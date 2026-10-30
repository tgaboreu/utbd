/*
 * utbd.c - UTC Time Broadcast Daemon
 *
 * A SIP user agent that answers incoming calls (registered with a SIP
 * PBX or provider, or directly from allowed addresses) and plays a time signal in the format of the NIST shortwave
 * stations WWV and WWVH (NIST Special Publication 250-67):
 *
 *   - 5 ms second ticks (1000 Hz WWV / 1200 Hz WWVH) in a 40 ms
 *     protected zone; the 29th and 59th ticks are omitted
 *   - 800 ms minute marker (1000 / 1200 Hz) and hour marker (1500 Hz)
 *   - 500 / 600 Hz tones on alternating minutes and a 440 Hz tone once
 *     an hour (not in hour 0), seconds 1-45, with the stations' silent
 *     minutes
 *   - the 100 Hz BCD time code (modified IRIG-H)
 *   - UT1 correction as doubled ticks
 *   - voice time announcement ("At the tone, ...") and station ID
 *
 * The signal is generated sample by sample from UTC, so every caller
 * hears the same broadcast.  Several calls can be served at once.
 *
 * Voice announcements are assembled from WAV files (see gen-voice.sh).
 *
 * Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/select.h>
#include <spandsp.h>

#include "sip.h"

/* ------------------------------------------------------------------ */
/*  Version and defaults                                                */
/* ------------------------------------------------------------------ */
#define UTBD_VERSION            "2.0.0"
#define DEFAULT_CONFIG_FILE     "/etc/utbd/utbd.conf"
#define DEFAULT_VOICE_DIR       "/usr/local/share/utbd/voice"
#define DEFAULT_SIP_PORT        5060
#define DEFAULT_RTP_MIN         10200
#define DEFAULT_RTP_MAX         10399
#define DEFAULT_MAX_CALLS       8
#define DEFAULT_MAX_CALL_SEC    180

#define MAX_CALLS_LIMIT         SIP_MAX_CALLS

/* ------------------------------------------------------------------ */
/*  Audio                                                               */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE             8000
#define FRAME_SAMPLES           160
#define FRAME_MS                20

/*
 * Levels as a fraction of full scale.  SP 250-67: ticks and markers
 * 100 %, voice 75 %, tones and time code 50 % modulation; time code
 * high:low amplitude ratio 3.3:1.  Scaled so the worst-case sum stays
 * below full scale.
 */
#define LEVEL_TICK              0.70
#define LEVEL_VOICE             0.52
#define LEVEL_TONE              0.35
#define LEVEL_CODE_HIGH         0.25
#define LEVEL_CODE_LOW          (LEVEL_CODE_HIGH / 3.3)

#define VOICE_MAX_SAMPLES       (SAMPLE_RATE * 45)

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */
typedef enum { STATION_WWV, STATION_WWVH } station_t;
typedef enum { DST_EU, DST_US, DST_NONE } dst_rules_t;

typedef struct {
    char        registrar[256];
    char        username[64];
    char        password[64];
    char        allow[256];
    char        local_ip[64];
    char        public_ip[64];
    int         sip_port;
    int         rtp_min, rtp_max;
    station_t   station;
    char        voice_dir[PATH_MAX];
    int         dut1;           /* UT1 - UTC in 0.1 s, -7..+7 */
    dst_rules_t dst;
    int         advance_ms;
    int         max_calls;
    int         max_call_sec;
    char        config_file[PATH_MAX];
    int         config_file_required;
} utbd_config_t;

static utbd_config_t g_cfg;

/* ------------------------------------------------------------------ */
/*  Logging — stdout, captured by the journal under systemd             */
/* ------------------------------------------------------------------ */
#define LOG(fmt, ...) \
do { \
    time_t _t = time(NULL); \
    struct tm _tm; \
    gmtime_r(&_t, &_tm); \
    fprintf(stdout, "[%04d-%02d-%02d %02d:%02d:%02dZ] " fmt "\n", \
            _tm.tm_year+1900, _tm.tm_mon+1, _tm.tm_mday, \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

/* ------------------------------------------------------------------ */
/*  Voice clips                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    float *s;           /* 8 kHz samples, peak-normalised to 1.0 */
    int    n;
} clip_t;

enum {
    W_AT_THE_TONE, W_HOUR, W_HOURS, W_MINUTE, W_MINUTES, W_UTC, W_STATION_ID,
    W_N0,                       /* W_N0 + 0..19 */
    W_N20 = W_N0 + 20, W_N30, W_N40, W_N50,
    W_COUNT
};

static const char *word_file[W_COUNT] = {
    [W_AT_THE_TONE] = "at_the_tone", [W_HOUR] = "hour", [W_HOURS] = "hours",
    [W_MINUTE] = "minute", [W_MINUTES] = "minutes", [W_UTC] = "utc",
    [W_STATION_ID] = "station_id",
    [W_N0 +  0] = "0",  [W_N0 +  1] = "1",  [W_N0 +  2] = "2",
    [W_N0 +  3] = "3",  [W_N0 +  4] = "4",  [W_N0 +  5] = "5",
    [W_N0 +  6] = "6",  [W_N0 +  7] = "7",  [W_N0 +  8] = "8",
    [W_N0 +  9] = "9",  [W_N0 + 10] = "10", [W_N0 + 11] = "11",
    [W_N0 + 12] = "12", [W_N0 + 13] = "13", [W_N0 + 14] = "14",
    [W_N0 + 15] = "15", [W_N0 + 16] = "16", [W_N0 + 17] = "17",
    [W_N0 + 18] = "18", [W_N0 + 19] = "19",
    [W_N20] = "20", [W_N30] = "30", [W_N40] = "40", [W_N50] = "50",
};

static clip_t g_words[W_COUNT];

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/*
 * Load a 16-bit PCM WAV file (any rate, first channel) and resample it
 * to 8 kHz.
 */
static int load_wav(const char *path, clip_t *c)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > 64L * 1024 * 1024) { fclose(f); return -1; }

    uint8_t *buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int rate = 0, channels = 0, bits = 0;
    const uint8_t *data = NULL;
    long data_len = 0;

    if (memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        long pos = 12;
        while (pos + 8 <= size) {
            uint32_t len = rd32(buf + pos + 4);
            if (len > (uint32_t)(size - pos - 8)) len = size - pos - 8;
            if (memcmp(buf + pos, "fmt ", 4) == 0 && len >= 16) {
                if (rd16(buf + pos + 8) != 1) break;        /* PCM only */
                channels = rd16(buf + pos + 10);
                rate     = rd32(buf + pos + 12);
                bits     = rd16(buf + pos + 22);
            } else if (memcmp(buf + pos, "data", 4) == 0) {
                data     = buf + pos + 8;
                data_len = len;
            }
            pos += 8 + len + (len & 1);
        }
    }

    if (!data || bits != 16 || channels < 1 || rate < 4000 || rate > 192000) {
        free(buf);
        return -1;
    }

    long in_n = data_len / (2 * channels);
    float *in = malloc(sizeof(float) * (in_n + 1));
    if (!in) { free(buf); return -1; }
    for (long i = 0; i < in_n; i++)
        in[i] = (int16_t)rd16(data + i * 2 * channels) / 32768.0f;
    free(buf);

    /*
     * Band-limited resampling: Blackman-windowed sinc interpolation with
     * the cut-off below the lower of the two Nyquist frequencies.  TTS
     * output has a lot of energy above 4 kHz; a cruder low-pass lets it
     * alias into the telephone band and the voice sounds metallic.
     */
    double ratio = (double)SAMPLE_RATE / rate;
    double fc    = 0.45 * (ratio < 1 ? ratio : 1);  /* cycles per input sample */
    double half  = 16 / (2 * fc);                   /* 16 zero crossings each side */
    long out_n = (long)(in_n * ratio);
    c->s = malloc(sizeof(float) * (out_n + 1));
    if (!c->s) { free(in); return -1; }

    float peak = 1e-6f;
    for (long i = 0; i < out_n; i++) {
        double x  = i / ratio;
        long   k0 = (long)ceil(x - half), k1 = (long)floor(x + half);
        double v  = 0;
        if (k0 < 0) k0 = 0;
        if (k1 > in_n - 1) k1 = in_n - 1;
        for (long k = k0; k <= k1; k++) {
            double d = x - k;
            double z = 2 * fc * d;
            double sinc = z == 0 ? 1 : sin(M_PI * z) / (M_PI * z);
            double u = d / half;
            double w = 0.42 + 0.5 * cos(M_PI * u) + 0.08 * cos(2 * M_PI * u);
            v += in[k] * 2 * fc * sinc * w;
        }
        c->s[i] = (float)v;
        if (fabsf(c->s[i]) > peak) peak = fabsf(c->s[i]);
    }
    for (long i = 0; i < out_n; i++)
        c->s[i] /= peak;

    /* Trim leading and trailing silence (TTS engines pad their output) */
    long a = 0, b = out_n;
    while (a < b && fabsf(c->s[a])     < 0.02f) a++;
    while (b > a && fabsf(c->s[b - 1]) < 0.02f) b--;
    a = a > 40 ? a - 40 : 0;
    b = b + 40 < out_n ? b + 40 : out_n;
    memmove(c->s, c->s + a, sizeof(float) * (b - a));

    c->n = (int)(b - a);
    free(in);
    return 0;
}

static int load_voice(void)
{
    int missing = 0;
    for (int i = 0; i < W_COUNT; i++) {
        char path[PATH_MAX + 32];
        snprintf(path, sizeof(path), "%s/%s.wav", g_cfg.voice_dir, word_file[i]);
        if (load_wav(path, &g_words[i]) < 0) {
            LOG("WARN: cannot load voice file %s", path);
            missing++;
        }
    }
    return missing;
}

/* ------------------------------------------------------------------ */
/*  Broadcast schedule (SP 250-67, figures 2.5 and 2.23)                */
/* ------------------------------------------------------------------ */
static int is_id_minute(int m)
{
    /* WWV: minute after the top / half hour; WWVH: minute before it */
    return g_cfg.station == STATION_WWV ? (m == 0 || m == 30)
                                        : (m == 29 || m == 59);
}

static int tone_freq(int h, int m)
{
    if (g_cfg.station == STATION_WWV) {
        if (m == 0 || m == 30 || m == 29 || m == 59 || (m >= 43 && m <= 51))
            return 0;
        if (m == 2 && h != 0) return 440;
        return (m % 2 == 0) ? 500 : 600;
    }
    if (m == 0 || m == 30 || m == 29 || m == 59 ||
        (m >= 8 && m <= 10) || (m >= 14 && m <= 19))
        return 0;
    if (m == 1 && h != 0) return 440;
    return (m % 2 == 0) ? 600 : 500;
}

/* ------------------------------------------------------------------ */
/*  Daylight saving time                                                */
/* ------------------------------------------------------------------ */
static time_t nth_sunday_utc(int year, int mon, int nth, int hour)
{
    struct tm tm = { 0 };
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon;
    tm.tm_mday = 1;
    time_t t = timegm(&tm);
    struct tm d;
    gmtime_r(&t, &d);
    int first_sunday = 1 + (7 - d.tm_wday) % 7;
    int mday;
    if (nth > 0) {
        mday = first_sunday + 7 * (nth - 1);
    } else {                                    /* last Sunday */
        int days_in_month = 31;
        tm.tm_mon  = mon + 1;
        tm.tm_mday = 0;
        time_t last = timegm(&tm);
        gmtime_r(&last, &d);
        days_in_month = d.tm_mday;
        mday = first_sunday + 7 * ((days_in_month - first_sunday) / 7);
    }
    tm.tm_mon  = mon;
    tm.tm_mday = mday;
    tm.tm_hour = hour;
    return timegm(&tm);
}

static int dst_in_effect(time_t t)
{
    struct tm u;
    gmtime_r(&t, &u);
    int y = u.tm_year + 1900;

    switch (g_cfg.dst) {
        case DST_EU:    /* last Sunday of March / October, 01:00 UTC */
            return t >= nth_sunday_utc(y, 2, -1, 1) &&
                   t <  nth_sunday_utc(y, 9, -1, 1);
        case DST_US:    /* 2nd Sunday of March / 1st of November, 02:00 Eastern */
            return t >= nth_sunday_utc(y, 2, 2, 7) &&
                   t <  nth_sunday_utc(y, 10, 1, 6);
        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Per-minute broadcast content                                        */
/* ------------------------------------------------------------------ */
static struct {
    time_t minute_start;
    int    width_ms[60];        /* 100 Hz pulse end per second, 0 = hole */
    int    tone;
    int    marker_hz;
    int    tick_hz;
    float  voice[VOICE_MAX_SAMPLES];
    int    voice_n;
    double voice_start;         /* seconds into the minute */
    int    id_n;                /* station ID length, from 1.0 s */
} gen;

static void set_bcd(int *bits, int first, int value, int nbits)
{
    for (int i = 0; i < nbits; i++)
        bits[first + i] = (value >> i) & 1;
}

static void build_time_code(time_t t, const struct tm *u)
{
    int bits[60] = { 0 };
    time_t day0 = t - (t % 86400);
    int doy = u->tm_yday + 1;
    int yr  = u->tm_year % 100;

    bits[2] = dst_in_effect(day0);              /* DST at 00:00 UTC today */
    bits[55] = dst_in_effect(day0 + 86400);     /* DST at 24:00 UTC today */
    /* bit 3: leap second warning — no source, left at 0 */
    set_bcd(bits,  4, yr % 10, 4);
    set_bcd(bits, 10, u->tm_min % 10, 4);
    set_bcd(bits, 15, u->tm_min / 10, 3);
    set_bcd(bits, 20, u->tm_hour % 10, 4);
    set_bcd(bits, 25, u->tm_hour / 10, 2);
    set_bcd(bits, 30, doy % 10, 4);
    set_bcd(bits, 35, (doy / 10) % 10, 4);
    set_bcd(bits, 40, doy / 100, 2);
    bits[50] = g_cfg.dut1 >= 0;
    set_bcd(bits, 51, yr / 10, 4);
    set_bcd(bits, 56, abs(g_cfg.dut1), 3);

    for (int s = 0; s < 60; s++)
        gen.width_ms[s] = (s % 10 == 9) ? 800 : bits[s] ? 500 : 200;
    gen.width_ms[0] = 0;                        /* frame reference "hole" */
}

static void voice_add(int w, double gap_after)
{
    const clip_t *c = &g_words[w];
    for (int i = 0; i < c->n && gen.voice_n < VOICE_MAX_SAMPLES; i++)
        gen.voice[gen.voice_n++] = c->s[i];
    for (int i = 0; i < gap_after * SAMPLE_RATE && gen.voice_n < VOICE_MAX_SAMPLES; i++)
        gen.voice[gen.voice_n++] = 0;
}

static void voice_number(int n, double gap_after)
{
    if (n < 20) {
        voice_add(W_N0 + n, gap_after);
    } else if (n % 10 == 0) {
        voice_add(W_N20 + n / 10 - 2, gap_after);
    } else {
        voice_add(W_N20 + n / 10 - 2, 0.03);
        voice_add(W_N0 + n % 10, gap_after);
    }
}

/* "At the tone, H hours, M minutes, Coordinated Universal Time." */
static void build_announcement(time_t next_minute)
{
    struct tm u;
    gmtime_r(&next_minute, &u);

    gen.voice_n = 0;
    for (int i = 0; i < W_COUNT; i++)
        if (i != W_STATION_ID && !g_words[i].n) return;

    voice_add(W_AT_THE_TONE, 0.20);
    voice_number(u.tm_hour, 0.06);
    voice_add(u.tm_hour == 1 ? W_HOUR : W_HOURS, 0.20);
    voice_number(u.tm_min, 0.06);
    voice_add(u.tm_min == 1 ? W_MINUTE : W_MINUTES, 0.20);
    voice_add(W_UTC, 0);
}

static void new_minute(time_t minute_start)
{
    struct tm u;
    gmtime_r(&minute_start, &u);

    gen.minute_start = minute_start;
    build_time_code(minute_start, &u);
    gen.tick_hz   = g_cfg.station == STATION_WWV ? 1000 : 1200;
    gen.marker_hz = u.tm_min == 0 ? 1500 : gen.tick_hz;
    gen.tone      = tone_freq(u.tm_hour, u.tm_min);
    gen.id_n      = is_id_minute(u.tm_min) ? g_words[W_STATION_ID].n : 0;
    if (gen.id_n > 43 * SAMPLE_RATE) gen.id_n = 43 * SAMPLE_RATE;

    /* WWV speaks about 7.5 s, WWVH about 15 s before the minute */
    gen.voice_start = g_cfg.station == STATION_WWV ? 52.5 : 45.0;
    build_announcement(minute_start + 60);
    int room = (int)((59.9 - gen.voice_start) * SAMPLE_RATE);
    if (gen.voice_n > room) gen.voice_n = room;
}

/* ------------------------------------------------------------------ */
/*  Signal at one instant (t_ns: UTC in nanoseconds)                    */
/* ------------------------------------------------------------------ */
static double synth(int64_t t_ns)
{
    time_t sec = (time_t)(t_ns / 1000000000LL);
    long   ns  = (long)(t_ns % 1000000000LL);
    time_t mstart = sec - sec % 60;
    if (mstart != gen.minute_start) new_minute(mstart);

    int    s  = (int)(sec % 60);
    double ms = ns / 1e6;
    double t  = s + ns / 1e9;           /* seconds into the minute */
    double v  = 0;

    /* UT1 correction: doubled ticks on seconds 1..8 (+) or 9..16 (-) */
    int doubled = (g_cfg.dut1 > 0 && s >= 1 && s <= g_cfg.dut1) ||
                  (g_cfg.dut1 < 0 && s >= 9 && s <= 8 - g_cfg.dut1);

    /* Protected zone: 10 ms before to 25 ms after each 5 ms tick */
    int quiet = ms >= 990 || ms < (doubled ? 130 : 30) || (s == 0 && ms < 800);

    if (s == 0) {
        if (ms < 800) v += LEVEL_TICK * sin(2 * M_PI * gen.marker_hz * t);
    } else if (s != 29 && s != 59) {
        if (ms < 5)
            v += LEVEL_TICK * sin(2 * M_PI * gen.tick_hz * ms / 1000);
        if (doubled && ms >= 100 && ms < 105)
            v += LEVEL_TICK * sin(2 * M_PI * gen.tick_hz * (ms - 100) / 1000);
    }

    if (!quiet) {
        double code = ms < gen.width_ms[s] ? LEVEL_CODE_HIGH : LEVEL_CODE_LOW;
        v += code * sin(2 * M_PI * 100 * t);

        if (gen.tone && t >= 1.0 && t < 45.0)
            v += LEVEL_TONE * sin(2 * M_PI * gen.tone * t);

        long i = (long)((t - 1.0) * SAMPLE_RATE);
        if (gen.id_n && i >= 0 && i < gen.id_n)
            v += LEVEL_VOICE * g_words[W_STATION_ID].s[i];

        i = (long)((t - gen.voice_start) * SAMPLE_RATE);
        if (i >= 0 && i < gen.voice_n)
            v += LEVEL_VOICE * gen.voice[i];
    }
    return v;
}

static void synth_frame(int64_t t_ns, int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) {
        double v = synth(t_ns + (int64_t)i * (1000000000LL / SAMPLE_RATE)) * 32767.0;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)lrint(v);
    }
}

/* ------------------------------------------------------------------ */
/*  --render: write the signal to a WAV file (no SIP needed)            */
/* ------------------------------------------------------------------ */
static void wr32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void wr16(FILE *f, uint16_t v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

static int render_wav(const char *path, int seconds, time_t start)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    uint32_t n = (uint32_t)seconds * SAMPLE_RATE;
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16); wr16(f, 1); wr16(f, 1); wr32(f, SAMPLE_RATE);
    wr32(f, SAMPLE_RATE * 2); wr16(f, 2); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, n * 2);

    int64_t t0 = (int64_t)start * 1000000000LL;
    int16_t pcm[FRAME_SAMPLES];
    for (uint32_t done = 0; done < n; done += FRAME_SAMPLES) {
        synth_frame(t0 + (int64_t)done * (1000000000LL / SAMPLE_RATE), pcm, FRAME_SAMPLES);
        for (int i = 0; i < FRAME_SAMPLES && done + i < n; i++)
            wr16(f, (uint16_t)pcm[i]);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Config file and command line                                        */
/* ------------------------------------------------------------------ */
static int set_option(utbd_config_t *cfg, const char *key, const char *val)
{
    if      (!strcmp(key, "registrar")) snprintf(cfg->registrar, sizeof(cfg->registrar), "%s", val);
    else if (!strcmp(key, "username"))  snprintf(cfg->username, sizeof(cfg->username), "%s", val);
    else if (!strcmp(key, "password"))  snprintf(cfg->password, sizeof(cfg->password), "%s", val);
    else if (!strcmp(key, "allow"))     snprintf(cfg->allow, sizeof(cfg->allow), "%s", val);
    else if (!strcmp(key, "local_ip"))  snprintf(cfg->local_ip, sizeof(cfg->local_ip), "%s", val);
    else if (!strcmp(key, "public_ip")) snprintf(cfg->public_ip, sizeof(cfg->public_ip), "%s", val);
    else if (!strcmp(key, "sip_port"))  cfg->sip_port = atoi(val);
    else if (!strcmp(key, "rtp_ports")) {
        if (sscanf(val, "%d-%d", &cfg->rtp_min, &cfg->rtp_max) != 2) return -1;
    } else if (!strcmp(key, "host") || !strcmp(key, "port") ||
               !strcmp(key, "peername") || !strcmp(key, "secret"))
        return -3;      /* IAX2 setting from 1.x */
    else if (!strcmp(key, "voice_dir")) snprintf(cfg->voice_dir, sizeof(cfg->voice_dir), "%s", val);
    else if (!strcmp(key, "dut1"))      cfg->dut1 = atoi(val);
    else if (!strcmp(key, "advance_ms"))   cfg->advance_ms   = atoi(val);
    else if (!strcmp(key, "max_calls"))    cfg->max_calls    = atoi(val);
    else if (!strcmp(key, "max_call_sec")) cfg->max_call_sec = atoi(val);
    else if (!strcmp(key, "station")) {
        if      (!strcmp(val, "wwv"))  cfg->station = STATION_WWV;
        else if (!strcmp(val, "wwvh")) cfg->station = STATION_WWVH;
        else return -1;
    } else if (!strcmp(key, "dst")) {
        if      (!strcmp(val, "eu"))   cfg->dst = DST_EU;
        else if (!strcmp(val, "us"))   cfg->dst = DST_US;
        else if (!strcmp(val, "none")) cfg->dst = DST_NONE;
        else return -1;
    } else {
        return -2;
    }
    return 0;
}

static int config_parse(utbd_config_t *cfg)
{
    FILE *f = fopen(cfg->config_file, "r");
    if (!f) {
        if (cfg->config_file_required) {
            fprintf(stderr, "ERROR: Cannot open config file %s: %s\n",
                    cfg->config_file, strerror(errno));
            return -1;
        }
        return 0;
    }

    char line[512];
    int  lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "WARNING: %s:%d: malformed line\n", cfg->config_file, lineno);
            continue;
        }
        *eq = '\0';
        char *k = p, *v = eq + 1;
        char *e = eq - 1;
        while (e >= k && (*e == ' ' || *e == '\t')) *e-- = '\0';
        while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v) - 1;
        while (e >= v && (*e == ' ' || *e == '\t')) *e-- = '\0';

        int rc = set_option(cfg, k, v);
        if (rc == -1)
            fprintf(stderr, "WARNING: %s:%d: invalid value for %s: %s\n",
                    cfg->config_file, lineno, k, v);
        else if (rc == -2)
            fprintf(stderr, "WARNING: %s:%d: unknown key: %s\n",
                    cfg->config_file, lineno, k);
        else if (rc == -3) {
            fprintf(stderr, "ERROR: %s:%d: '%s' is an IAX2 setting; utbd 2.x "
                    "uses SIP (see README.md, 'Upgrading from 1.x')\n",
                    cfg->config_file, lineno, k);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static void print_help(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS]\n"
        "\n"
        "UTC Time Broadcast Daemon — answers SIP calls and plays a\n"
        "WWV / WWVH format time signal.\n"
        "\n"
        "Options:\n"
        "  -c, --config=FILE        Configuration file (default: %s)\n"
        "  -R, --registrar=HOST     SIP PBX or provider to register with\n"
        "  -u, --username=USER      SIP user name\n"
        "  -w, --password=PASS      SIP password\n"
        "  -a, --allow=LIST         Addresses allowed to call: 'any' or\n"
        "                           IPs / CIDRs (default: the registrar)\n"
        "  -p, --sip-port=PORT      Local SIP port (default: %d)\n"
        "  -S, --station=STATION    wwv or wwvh (default: wwv)\n"
        "  -v, --voice-dir=DIR      Voice WAV files (default: %s)\n"
        "  -r, --render=FILE        Write the signal to a WAV file and exit\n"
        "  -d, --duration=SECS      Length of --render output (default: 120)\n"
        "  -t, --start=TIME         --render start, \"YYYY-MM-DD HH:MM\" UTC\n"
        "                           (default: the current minute)\n"
        "  -h, --help               This help\n"
        "  -V, --version            Version\n"
        "\n"
        "All configuration file keys: registrar, username, password, allow,\n"
        "local_ip, public_ip, sip_port, rtp_ports, station, voice_dir, dut1,\n"
        "dst (eu|us|none), advance_ms, max_calls, max_call_sec.\n"
        "\n",
        prog, DEFAULT_CONFIG_FILE, DEFAULT_SIP_PORT, DEFAULT_VOICE_DIR);
}

static char g_render_file[PATH_MAX];
static int  g_render_seconds = 120;
static time_t g_render_start;

static int cli_parse(int argc, char *argv[], utbd_config_t *cfg)
{
    static const struct option opts[] = {
        { "config",    required_argument, NULL, 'c' },
        { "registrar", required_argument, NULL, 'R' },
        { "username",  required_argument, NULL, 'u' },
        { "password",  required_argument, NULL, 'w' },
        { "allow",     required_argument, NULL, 'a' },
        { "sip-port",  required_argument, NULL, 'p' },
        { "station",   required_argument, NULL, 'S' },
        { "voice-dir", required_argument, NULL, 'v' },
        { "render",    required_argument, NULL, 'r' },
        { "duration",  required_argument, NULL, 'd' },
        { "start",     required_argument, NULL, 't' },
        { "help",      no_argument,       NULL, 'h' },
        { "version",   no_argument,       NULL, 'V' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:R:u:w:a:p:S:v:r:d:t:hV", opts, NULL)) != -1) {
        int rc = 0;
        switch (opt) {
            case 'c':
                snprintf(cfg->config_file, sizeof(cfg->config_file), "%s", optarg);
                cfg->config_file_required = 1;
                break;
            case 'R': rc = set_option(cfg, "registrar", optarg); break;
            case 'u': rc = set_option(cfg, "username",  optarg); break;
            case 'w': rc = set_option(cfg, "password",  optarg); break;
            case 'a': rc = set_option(cfg, "allow",     optarg); break;
            case 'p': rc = set_option(cfg, "sip_port",  optarg); break;
            case 'S': rc = set_option(cfg, "station",   optarg); break;
            case 'v': rc = set_option(cfg, "voice_dir", optarg); break;
            case 'r': snprintf(g_render_file, sizeof(g_render_file), "%s", optarg); break;
            case 'd': g_render_seconds = atoi(optarg); break;
            case 't': {
                struct tm tm = { 0 };
                if (sscanf(optarg, "%d-%d-%d %d:%d", &tm.tm_year, &tm.tm_mon,
                           &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5) {
                    rc = -1;
                    break;
                }
                tm.tm_year -= 1900;
                tm.tm_mon  -= 1;
                g_render_start = timegm(&tm);
                break;
            }
            case 'h': print_help(argv[0]); exit(EXIT_SUCCESS);
            case 'V':
                printf("utbd %s\n"
                       "Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>\n"
                       "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
                       "This is free software: you are free to change and redistribute it.\n"
                       "There is NO WARRANTY, to the extent permitted by law.\n"
                       "\n"
                       "Libraries:\n"
                       "  spandsp    (LGPL-2.1)          https://github.com/freeswitch/spandsp\n"
                       "  Sofia-SIP  (LGPL-2.1-or-later) https://github.com/freeswitch/sofia-sip\n",
                       UTBD_VERSION);
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Try '%s --help'.\n", argv[0]);
                return -1;
        }
        if (rc < 0) {
            fprintf(stderr, "ERROR: invalid value: %s\n", optarg);
            return -1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "ERROR: Unexpected argument: %s\n", argv[optind]);
        return -1;
    }
    return 0;
}

static int config_validate(const utbd_config_t *cfg, int need_sip)
{
    int ok = 1;
    if (need_sip) {
        if (!cfg->registrar[0] && !cfg->allow[0])
            { fprintf(stderr, "ERROR: set 'registrar' (register with a PBX) and/or "
                              "'allow' (accept calls from these addresses).\n"); ok = 0; }
        if (cfg->registrar[0] && !cfg->username[0])
            { fprintf(stderr, "ERROR: 'username' is required with a registrar.\n"); ok = 0; }
    }
    if (cfg->sip_port < 1 || cfg->sip_port > 65535)
        { fprintf(stderr, "ERROR: 'sip_port' must be 1..65535.\n");            ok = 0; }
    if (cfg->rtp_min < 1024 || cfg->rtp_max > 65535 || cfg->rtp_max <= cfg->rtp_min)
        { fprintf(stderr, "ERROR: 'rtp_ports' must be a range within 1024-65535.\n"); ok = 0; }
    if (cfg->dut1 < -7 || cfg->dut1 > 7)
        { fprintf(stderr, "ERROR: 'dut1' must be -7..7 (tenths of a second).\n"); ok = 0; }
    if (cfg->advance_ms < 0 || cfg->advance_ms > 1000)
        { fprintf(stderr, "ERROR: 'advance_ms' must be 0..1000.\n");           ok = 0; }
    if (cfg->max_calls < 1 || cfg->max_calls > MAX_CALLS_LIMIT)
        { fprintf(stderr, "ERROR: 'max_calls' must be 1..%d.\n", MAX_CALLS_LIMIT); ok = 0; }
    if (cfg->max_call_sec < 10)
        { fprintf(stderr, "ERROR: 'max_call_sec' must be >= 10.\n");           ok = 0; }
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  SIP                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    sip_call_t *call;
    time_t      start;
} call_t;

static struct {
    call_t              calls[MAX_CALLS_LIMIT];
    int                 ncalls;
    struct timespec     next_tx;        /* CLOCK_MONOTONIC */
    int64_t             next_tx_utc_ns; /* audio time of the next frame */
} g;

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static void ts_add_ms(struct timespec *ts, long ms)
{
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static long ms_until(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (deadline->tv_sec  - now.tv_sec)  * 1000L
         + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}

static int find_call(sip_call_t *c)
{
    for (int i = 0; i < g.ncalls; i++)
        if (g.calls[i].call == c) return i;
    return -1;
}

static void remove_call(int i, const char *why)
{
    LOG("Call ended (%s) after %lds, %d active",
        why, (long)(time(NULL) - g.calls[i].start), g.ncalls - 1);
    g.calls[i] = g.calls[--g.ncalls];
}

static void on_sip_log(const char *msg)
{
    LOG("%s", msg);
}

/* Caller ID arrives sanitised (printable ASCII, bounded) from sip.c */
static int on_sip_incoming(sip_call_t *c, const char *caller)
{
    if (g.ncalls >= g_cfg.max_calls) {
        LOG("Rejected call from %s: %d calls active", caller, g.ncalls);
        return 486;
    }

    if (g.ncalls == 0) {
        /* Start the shared audio clock */
        struct timespec rt;
        clock_gettime(CLOCK_MONOTONIC, &g.next_tx);
        clock_gettime(CLOCK_REALTIME, &rt);
        int64_t now = (int64_t)rt.tv_sec * 1000000000LL + rt.tv_nsec;
        g.next_tx_utc_ns = now + (int64_t)g_cfg.advance_ms * 1000000LL;
    }

    g.calls[g.ncalls].call  = c;
    g.calls[g.ncalls].start = time(NULL);
    g.ncalls++;
    LOG("Call from %s answered, %d active", caller, g.ncalls);
    return 0;
}

static void on_sip_ended(sip_call_t *c, const char *why)
{
    int i = find_call(c);
    if (i >= 0) remove_call(i, why);
}

/* One frame of the broadcast, sent to every caller */
static void send_frame(void)
{
    int16_t pcm[FRAME_SAMPLES];

    synth_frame(g.next_tx_utc_ns, pcm, FRAME_SAMPLES);
    for (int i = 0; i < g.ncalls; i++)
        sip_send_audio(g.calls[i].call, pcm, FRAME_SAMPLES);

    g.next_tx_utc_ns += (int64_t)FRAME_MS * 1000000LL;
}

static void run_daemon(void)
{
    sip_config_t sc = {
        .port = g_cfg.sip_port, .registrar = g_cfg.registrar, .do_register = 1,
        .username = g_cfg.username, .password = g_cfg.password,
        .local_ip = g_cfg.local_ip, .public_ip = g_cfg.public_ip,
        .allow = g_cfg.allow,
        .rtp_port_min = g_cfg.rtp_min, .rtp_port_max = g_cfg.rtp_max,
        .user_agent = "utbd/" UTBD_VERSION,
    };
    sip_handlers_t sh = {
        .on_incoming = on_sip_incoming, .on_ended = on_sip_ended,
        .log = on_sip_log,          /* received audio is not used */
    };
    if (sip_init(&sc, &sh) < 0)
        return;

    while (g_running) {
        /* Calls over the time limit */
        for (int i = g.ncalls - 1; i >= 0; i--) {
            if (time(NULL) - g.calls[i].start >= g_cfg.max_call_sec) {
                sip_hangup(g.calls[i].call);
                remove_call(i, "time limit");
            }
        }

        /* Fixed-rate TX shared by all calls */
        if (g.ncalls > 0) {
            for (int i = 0; i < 3 && ms_until(&g.next_tx) <= 0; i++) {
                send_frame();
                ts_add_ms(&g.next_tx, FRAME_MS);
            }
            if (ms_until(&g.next_tx) < -FRAME_MS) {
                /* Fell behind: realign audio time with the wall clock */
                long behind = -ms_until(&g.next_tx);
                clock_gettime(CLOCK_MONOTONIC, &g.next_tx);
                g.next_tx_utc_ns += (int64_t)behind * 1000000LL;
            }
        }

        int wait_ms = FRAME_MS;
        if (g.ncalls > 0) {
            long tx = ms_until(&g.next_tx);
            if (tx < wait_ms) wait_ms = tx < 0 ? 0 : (int)tx;
        }
        sip_poll(wait_ms);
    }

    sip_shutdown();             /* hangs up and unregisters */
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.config_file, sizeof(g_cfg.config_file), "%s", DEFAULT_CONFIG_FILE);
    snprintf(g_cfg.voice_dir, sizeof(g_cfg.voice_dir), "%s", DEFAULT_VOICE_DIR);
    g_cfg.sip_port     = DEFAULT_SIP_PORT;
    g_cfg.rtp_min      = DEFAULT_RTP_MIN;
    g_cfg.rtp_max      = DEFAULT_RTP_MAX;
    g_cfg.station      = STATION_WWV;
    g_cfg.dst          = DST_EU;
    g_cfg.max_calls    = DEFAULT_MAX_CALLS;
    g_cfg.max_call_sec = DEFAULT_MAX_CALL_SEC;

    /* First CLI pass finds --config; the second overrides the file */
    if (cli_parse(argc, argv, &g_cfg) < 0) return EXIT_FAILURE;
    if (config_parse(&g_cfg) < 0)          return EXIT_FAILURE;
    optind = 0;
    if (cli_parse(argc, argv, &g_cfg) < 0) return EXIT_FAILURE;

    if (config_validate(&g_cfg, !g_render_file[0]) < 0) return EXIT_FAILURE;

    LOG("utbd %s starting: station=%s dst=%s dut1=%+d advance=%dms voice=%s",
        UTBD_VERSION, g_cfg.station == STATION_WWV ? "WWV" : "WWVH",
        g_cfg.dst == DST_EU ? "eu" : g_cfg.dst == DST_US ? "us" : "none",
        g_cfg.dut1, g_cfg.advance_ms, g_cfg.voice_dir);

    int missing = load_voice();
    if (missing)
        LOG("WARN: %d voice file(s) missing - announcements disabled "
            "(run gen-voice.sh)", missing);

    if (g_render_file[0]) {
        time_t start = g_render_start ? g_render_start : time(NULL);
        start -= start % 60;            /* begin at a minute marker */
        if (render_wav(g_render_file, g_render_seconds, start) < 0)
            return EXIT_FAILURE;
        LOG("Rendered %ds to %s", g_render_seconds, g_render_file);
        return EXIT_SUCCESS;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    LOG("SIP port %d, RTP %d-%d, registrar %s as %s, allowed callers: %s",
        g_cfg.sip_port, g_cfg.rtp_min, g_cfg.rtp_max,
        g_cfg.registrar[0] ? g_cfg.registrar : "(none)",
        g_cfg.username[0] ? g_cfg.username : "-",
        g_cfg.allow[0] ? g_cfg.allow : "registrar only");
    LOG("Up to %d calls of %ds", g_cfg.max_calls, g_cfg.max_call_sec);

    run_daemon();
    LOG("utbd stopped");
    return EXIT_SUCCESS;
}
