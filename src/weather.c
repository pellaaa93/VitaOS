/* The weather widget, from Open-Meteo (free, no key), refreshed every 30
 * minutes on its own thread, for the town picked in Settings > Weather
 * location (user/weather.cfg: "lat lon F|C name"). No town, no widget. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "weather.h"

#define CFG "ux0:data/arcadehub/user/weather.cfg"
#define URL "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
            "&current=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min" \
            "&temperature_unit=%s&timezone=auto&forecast_days=4"

static char place[64];
static volatile int stale;                            /* the town changed: fetch now */
static SceUID weather_sema = -1;
static WeatherDay days_forecast[4];
static int is_fahrenheit;


/* The town, from user/weather.cfg; 0 when none is set. */
static int read_cfg(float *lat, float *lon, int *fahrenheit) {
    char b[160] = {0};
    SceUID fd = sceIoOpen(CFG, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int rd = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (rd <= 0) return 0;
    b[rd] = 0;

    char unit = 'C';
    char town[64] = {0};
    float t_lat = 0, t_lon = 0;
    if (sscanf(b, "%f %f %c %63[^\r\n]", &t_lat, &t_lon, &unit, town) < 4) return 0;
    *lat = t_lat;
    *lon = t_lon;
    *fahrenheit = (unit == 'F');
    is_fahrenheit = *fahrenheit;

    char *p = town;
    while (*p == ' ' || *p == '\t') p++;
    snprintf(place, sizeof(place), "%s", p);
    for (char *q = place; *q; ++q) if (*q == '\n' || *q == '\r') *q = 0;
    return 1;
}

static volatile int ready, temp, hi, lo, code;
static char body[16384];
static int used;

static size_t on_data(char *p, size_t s, size_t n, void *u) {
    (void)u;
    size_t k = s * n;
    if (used + k >= sizeof(body)) k = sizeof(body) - 1 - used;
    memcpy(body + used, p, k);
    used += k;
    body[used] = 0;
    return s * n;
}

/* The number after "key": inside the object that starts at 'section'. */
static float number_after(const char *section, const char *key) {
    const char *s = body;
    if (section && *section) {
        s = strstr(body, section);
        if (!s) return -999;
        const char *brace = strchr(s, '{');
        if (brace) s = brace;
    }
    const char *k = strstr(s, key);
    if (!k) return -999;
    k = strchr(k, ':');
    if (!k) return -999;
    while (*k == ':' || *k == '[' || *k == ' ' || *k == '\t') ++k;
    return strtof(k, NULL);
}

/* Parse an array of floats after "key": inside the section. Returns count found. */
static int array_after(const char *section, const char *key, float out[], int max_count) {
    const char *s = body;
    if (section && *section) {
        s = strstr(body, section);
        if (!s) return 0;
    }
    const char *k = strstr(s, key);
    if (!k) return 0;
    const char *bracket = strchr(k, '[');
    if (!bracket) return 0;
    const char *p = bracket + 1;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') p++;
        if (*p == ']' || !*p) break;
        char *next = NULL;
        float val = strtof(p, &next);
        if (next == p) break;
        out[count++] = val;
        p = next;
    }
    return count;
}

static int worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        float lat = 0, lon = 0;
        int f = 1;
        stale = 0;
        if (!read_cfg(&lat, &lon, &f)) {                 /* no town yet: nothing to show */
            ready = 0;
            SceUInt timeout = 60 * 1000 * 1000;
            if (weather_sema >= 0) sceKernelWaitSemaCB(weather_sema, 1, &timeout);
            else for (int i = 0; i < 60 && !stale; ++i) sceKernelDelayThread(1000 * 1000);
            continue;
        }
        is_fahrenheit = f;
        char url[320];
        snprintf(url, sizeof(url), URL, lat, lon, f ? "fahrenheit" : "celsius");
        CURL *c = curl_easy_init();
        if (c) {
            used = 0; body[0] = 0;
            curl_easy_setopt(c, CURLOPT_URL, url);
            curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
            curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.0 (PS Vita)");
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
            if (curl_easy_perform(c) == CURLE_OK) {
                float t = number_after("\"current\"", "\"temperature_2m\"");
                float w = number_after("\"current\"", "\"weather_code\"");
                float d_codes[4] = {0}, d_max[4] = {0}, d_min[4] = {0};
                int nc = array_after("\"daily\"", "\"weather_code\"", d_codes, 4);
                int nh = array_after("\"daily\"", "\"temperature_2m_max\"", d_max, 4);
                int nl = array_after("\"daily\"", "\"temperature_2m_min\"", d_min, 4);
                if (t > -200) {
                    temp = (int)(t + (t < 0 ? -0.5f : 0.5f));
                    code = (w > -900) ? (int)w : 0;
                    hi = (nh > 0) ? (int)(d_max[0] + (d_max[0] < 0 ? -0.5f : 0.5f)) : temp;
                    lo = (nl > 0) ? (int)(d_min[0] + (d_min[0] < 0 ? -0.5f : 0.5f)) : temp;
                    for (int i = 0; i < 4; ++i) {
                        days_forecast[i].code = (i < nc) ? (int)d_codes[i] : code;
                        days_forecast[i].temp_max = (i < nh) ? (int)(d_max[i] + (d_max[i] < 0 ? -0.5f : 0.5f)) : temp;
                        days_forecast[i].temp_min = (i < nl) ? (int)(d_min[i] + (d_min[i] < 0 ? -0.5f : 0.5f)) : temp;
                    }
                    ready = 1;
                }
            }
            curl_easy_cleanup(c);
        }
        unsigned int wait_s = ready ? 30 * 60 : 60;       /* retry in 60s if failed, else 30 min */
        SceUInt timeout = wait_s * 1000 * 1000;
        if (weather_sema >= 0) sceKernelWaitSemaCB(weather_sema, 1, &timeout);
        else for (unsigned int i = 0; i < wait_s && !stale; ++i) sceKernelDelayThread(1000 * 1000);
    }
    return 0;
}

void weather_init(void) {
    sceIoMkdir("ux0:data/arcadehub", 0777);
    sceIoMkdir("ux0:data/arcadehub/user", 0777);

    float lat = 0, lon = 0;
    int f = 0;
    read_cfg(&lat, &lon, &f);

    if (weather_sema < 0) weather_sema = sceKernelCreateSema("weather_wake", 0, 0, 1, NULL);

    SceUID t = sceKernelCreateThread("weather", worker, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* WMO weather codes, in words. */
static const char *words(int c) {
    if (c == 0) return "Clear";
    if (c <= 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return c >= 65 ? "Heavy rain" : "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c >= 85 && c <= 86) return "Snow showers";
    if (c >= 95) return "Thunderstorms";
    return "";
}

const char *weather_desc(int c) {
    return words(c);
}

int weather_kind(int c) {
    if (c == 0) return 0;
    if (c <= 3 || c == 45 || c == 48) return 1;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return 3;
    return 2;
}

int weather_line(char *out, int max, char *sub, int submax, int *kind) {
    if (!ready) return 0;
    *kind = weather_kind(code);
    snprintf(out, max, "%d\xC2\xB0  %s", temp, words(code));
    snprintf(sub, submax, "%s  \xC2\xB7  H %d\xC2\xB0  L %d\xC2\xB0", place, hi, lo);
    return 1;
}

int weather_forecast(WeatherDay days[4], char town[64], int *fahrenheit, int *curr_temp, int *curr_code) {
    if (!ready) return 0;
    if (days) memcpy(days, days_forecast, sizeof(WeatherDay) * 4);
    if (town) snprintf(town, 64, "%s", place);
    if (fahrenheit) *fahrenheit = is_fahrenheit;
    if (curr_temp) *curr_temp = temp;
    if (curr_code) *curr_code = code;
    return 1;
}


/* Settings > Weather location: search Open-Meteo's geocoder, then save.
 * Returns the number of matches written to names/coords (up to max). */
static char sbody[16384];
static int sused;
static size_t on_search(char *p, size_t s, size_t n, void *u) {
    (void)u;
    size_t k = s * n;
    if (sused + k >= sizeof(sbody)) k = sizeof(sbody) - 1 - sused;
    memcpy(sbody + sused, p, k);
    sused += k;
    sbody[sused] = 0;
    return s * n;
}

int weather_search(const char *query, char names[][96], float *lat, float *lon, int *us, int max) {
    char q[160];
    int n = 0;
    for (const char *p = query; *p && n < (int)sizeof(q) - 4; ++p) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) q[n++] = ch;
        else n += snprintf(q + n, sizeof(q) - n, "%%%02X", ch);
    }
    q[n] = 0;
    char url[256];
    snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?count=%d&language=en&name=%s", max, q);
    CURL *c = curl_easy_init();
    if (!c) return 0;
    sused = 0; sbody[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_search);
    int ok = curl_easy_perform(c) == CURLE_OK;
    curl_easy_cleanup(c);
    if (!ok) return -1;
    int k = 0;
    for (const char *r = strstr(sbody, "\"name\""); r && k < max; r = strstr(r + 6, "\"name\"")) {
        char name[48] = "", admin[48] = "", country[48] = "";
        const char *end = strchr(r, '}');
        sscanf(r, "\"name\":\"%47[^\"]", name);
        const char *a = strstr(r, "\"admin1\":\""), *co = strstr(r, "\"country\":\""), *cc = strstr(r, "\"country_code\":\"");
        const char *la = strstr(r, "\"latitude\":"), *lo2 = strstr(r, "\"longitude\":");
        if (a && end && a < end) sscanf(a, "\"admin1\":\"%47[^\"]", admin);
        if (co && end && co < end) sscanf(co, "\"country\":\"%47[^\"]", country);
        if (!la || !lo2) break;
        lat[k] = strtof(la + 11, NULL);
        lon[k] = strtof(lo2 + 12, NULL);
        us[k] = cc && end && cc < end && !strncmp(cc + 16, "US", 2);
        snprintf(names[k], 96, "%s%s%s%s%s", name, *admin ? ", " : "", admin, *country ? ", " : "", country);
        ++k;
    }
    return k;
}

void weather_set(const char *name, float lat, float lon, int fahrenheit) {
    char line[160];
    char shortname[64];
    snprintf(shortname, sizeof(shortname), "%s", name);
    char *comma = strchr(shortname, ',');
    if (comma) *comma = 0;                            /* the widget shows the town only */

    /* Update place immediately so Settings and widgets reflect it right away */
    char *p = shortname;
    while (*p == ' ' || *p == '\t') p++;
    snprintf(place, sizeof(place), "%s", p);

    sceIoMkdir("ux0:data/arcadehub", 0777);
    sceIoMkdir("ux0:data/arcadehub/user", 0777);

    int n = snprintf(line, sizeof(line), "%.4f %.4f %c %s\n", lat, lon, fahrenheit ? 'F' : 'C', place);
    SceUID fd = sceIoOpen(CFG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }

    is_fahrenheit = fahrenheit;
    ready = 0;
    stale = 1;
    if (weather_sema >= 0) sceKernelSignalSema(weather_sema, 1);
}

void weather_off(void) {
    sceIoRemove(CFG);
    place[0] = 0;
    memset(days_forecast, 0, sizeof(days_forecast));
    ready = 0;
    stale = 1;
    if (weather_sema >= 0) sceKernelSignalSema(weather_sema, 1);
}

const char *weather_place(void) { return place; }
