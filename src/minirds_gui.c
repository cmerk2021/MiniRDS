/*
 * MiniRDS GUI - Windows graphical frontend for MiniRDS
 *
 * Provides a comprehensive GUI for controlling the MiniRDS RDS encoder
 * with real-time parameter adjustment, file-watched inputs for RT/PS/RT+,
 * audio device selection, command file loading, and live RDS monitor.
 *
 * Copyright (C) 2024 MiniRDS Contributors
 * License: GPLv3
 */

#ifdef _WIN32

#include "common.h"
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <fcntl.h>

#include <ao/ao.h>

#include "rds.h"
#include "fm_mpx.h"
#include "resampler.h"
#include "modulator.h"
#include "lib.h"
#include "ascii_cmd.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")
#endif

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define APP_TITLE       "MiniRDS Control Panel"
#define WINDOW_WIDTH    660
#define WINDOW_HEIGHT   988

/* Control IDs */
enum {
    IDC_FIRST = 100,
    /* RDS Settings */
    IDC_PI_EDIT, IDC_PS_EDIT, IDC_RT_EDIT, IDC_PTY_EDIT, IDC_PTYN_EDIT,
    IDC_TP_CHECK, IDC_TA_CHECK, IDC_MS_CHECK,
    IDC_AF_EDIT, IDC_LPS_EDIT, IDC_ERT_EDIT,
    IDC_APPLY_BTN,
    /* Audio */
    IDC_DEVICE_COMBO, IDC_VOLUME_SLIDER, IDC_VOLUME_LABEL,
    /* Control */
    IDC_START_BTN, IDC_STOP_BTN, IDC_STATUS_LABEL,
    IDC_FILE_EDIT, IDC_BROWSE_BTN, IDC_EXEC_BTN,
    /* File watch inputs */
    IDC_RT_FILE_EDIT, IDC_RT_FILE_BROWSE, IDC_RT_FILE_ACTIVE,
    IDC_PS_FILE_EDIT, IDC_PS_FILE_BROWSE, IDC_PS_FILE_ACTIVE,
    IDC_RTP_FILE_EDIT, IDC_RTP_FILE_BROWSE, IDC_RTP_FILE_ACTIVE,
    IDC_PT_FILE_EDIT, IDC_PT_FILE_BROWSE, IDC_PT_FILE_ACTIVE,
    /* Monitor labels */
    IDC_MON_PI, IDC_MON_PS, IDC_MON_RT, IDC_MON_PTY, IDC_MON_PTYN,
    IDC_MON_TP, IDC_MON_TA, IDC_MON_MS, IDC_MON_LPS, IDC_MON_ERT,
    IDC_MON_RTP1, IDC_MON_RTP2, IDC_MON_AF,
    /* Log */
    IDC_LOG_EDIT,
};

#define IDT_LOG_TIMER       1
#define IDT_FILEWATCH_TIMER 2
#define IDT_MONITOR_TIMER   3
#define LOG_TIMER_MS        100
#define FILEWATCH_TIMER_MS  500
#define MONITOR_TIMER_MS    250

/* -----------------------------------------------------------------------
 * Global State
 * ----------------------------------------------------------------------- */

static HWND g_hwnd = NULL;
static HWND g_log_edit = NULL;
static HFONT g_font = NULL;
static HFONT g_mono_font = NULL;
static HFONT g_bold_font = NULL;

/* Engine state */
static volatile LONG g_engine_running = 0;
static volatile LONG g_stop_engine = 0;
static HANDLE g_engine_thread = NULL;

/* Audio device list */
#define MAX_AUDIO_DEVICES 32
static struct {
    UINT id;
    char name[MAXPNAMELEN];
} g_audio_devices[MAX_AUDIO_DEVICES];
static int g_num_audio_devices = 0;
static int g_selected_device = -1;
static float g_volume = 50.0f;

/* stderr capture pipe */
static HANDLE g_stderr_read = NULL;
static HANDLE g_stderr_write = NULL;

/* File watch state */
static struct {
    char path[MAX_PATH];
    FILETIME last_write;
    BOOL active;
} g_rt_file, g_ps_file, g_rtp_file, g_pt_file;

/* PS chunking state */
static struct {
    char full_text[512];
    char chunks[64][PS_LENGTH + 1]; /* up to 64 chunks of 8 chars */
    int num_chunks;
    int current_chunk;
    DWORD last_advance_tick;
} g_ps_scroll;

#define PS_SCROLL_INTERVAL_MS 4000 /* advance PS chunk every 4 seconds */

/* -----------------------------------------------------------------------
 * Logging
 * ----------------------------------------------------------------------- */

static void append_log(const char *text) {
    if (!g_log_edit) return;
    int len = GetWindowTextLengthA(g_log_edit);
    if (len > 32000) {
        SendMessageA(g_log_edit, EM_SETSEL, 0, len - 16000);
        SendMessageA(g_log_edit, EM_REPLACESEL, FALSE, (LPARAM)"[...trimmed...]\r\n");
        len = GetWindowTextLengthA(g_log_edit);
    }
    SendMessageA(g_log_edit, EM_SETSEL, len, len);
    SendMessageA(g_log_edit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
}

static void log_msg(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    append_log(buf);
}

static int setup_stderr_capture(void) {
    SECURITY_ATTRIBUTES sa;
    int fd;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&g_stderr_read, &g_stderr_write, &sa, 0))
        return -1;

    fd = _open_osfhandle((intptr_t)g_stderr_write, _O_WRONLY);
    if (fd < 0) return -1;

    _dup2(fd, _fileno(stderr));
    setvbuf(stderr, NULL, _IONBF, 0);

    return 0;
}

static void drain_stderr_to_log(void) {
    DWORD available = 0;
    char buf[4096];
    DWORD bytes_read;

    if (!g_stderr_read) return;

    while (PeekNamedPipe(g_stderr_read, NULL, 0, NULL, &available, NULL)
           && available > 0) {
        DWORD to_read = (available < sizeof(buf) - 1) ? available : sizeof(buf) - 1;
        if (ReadFile(g_stderr_read, buf, to_read, &bytes_read, NULL) && bytes_read > 0) {
            buf[bytes_read] = '\0';
            char converted[8192];
            int j = 0;
            for (DWORD i = 0; i < bytes_read && j < (int)sizeof(converted) - 2; i++) {
                if (buf[i] == '\n' && (i == 0 || buf[i-1] != '\r'))
                    converted[j++] = '\r';
                converted[j++] = buf[i];
            }
            converted[j] = '\0';
            append_log(converted);
        } else {
            break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Audio Device Enumeration
 * ----------------------------------------------------------------------- */

static void enumerate_audio_devices(HWND combo) {
    UINT num_devs = waveOutGetNumDevs();
    WAVEOUTCAPSA caps;

    g_num_audio_devices = 0;
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"(System Default)");

    for (UINT i = 0; i < num_devs && g_num_audio_devices < MAX_AUDIO_DEVICES; i++) {
        if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            g_audio_devices[g_num_audio_devices].id = i;
            snprintf(g_audio_devices[g_num_audio_devices].name,
                     MAXPNAMELEN, "%s", caps.szPname);
            char label[128];
            snprintf(label, sizeof(label), "%u: %s", i, caps.szPname);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)label);
            g_num_audio_devices++;
        }
    }
    SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

/* -----------------------------------------------------------------------
 * Sample format conversion
 * ----------------------------------------------------------------------- */

static inline void float2char2channel(
    float *inbuf, char *outbuf, size_t frames) {
    uint16_t j = 0, k = 0;
    int16_t sample;
    int8_t lower, upper;

    for (uint16_t i = 0; i < frames; i++) {
        sample = lroundf((inbuf[j] + inbuf[j+1]) * 16383.5f);
        lower = sample & 255;
        sample >>= 8;
        upper = sample & 255;
        outbuf[k+0] = lower;
        outbuf[k+1] = upper;
        outbuf[k+2] = lower;
        outbuf[k+3] = upper;
        j += 2;
        k += 4;
    }
}

/* -----------------------------------------------------------------------
 * PS Text Chunking (word-per-line)
 *
 * Splits text into 8-character PS chunks, one word per chunk.
 * Each word gets its own chunk (padded with spaces).
 * Only words longer than 8 characters are truncated with a dash.
 * ----------------------------------------------------------------------- */

static void ps_chunk_text(const char *text) {
    int ci = 0;
    char copy[512];
    char *words[256];
    int nwords = 0;

    size_t len = strlen(text);
    if (len == 0) {
        g_ps_scroll.num_chunks = 0;
        return;
    }

    /* Make a mutable copy and trim trailing whitespace */
    strncpy(copy, text, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    len = strlen(copy);
    while (len > 0 && (copy[len-1] == ' ' || copy[len-1] == '\t' ||
           copy[len-1] == '\n' || copy[len-1] == '\r'))
        copy[--len] = '\0';

    /* Tokenize into words */
    {
        char *tok = strtok(copy, " \t");
        while (tok && nwords < 256) {
            words[nwords++] = tok;
            tok = strtok(NULL, " \t");
        }
    }

    /* Each word becomes one or more chunks */
    for (int w = 0; w < nwords && ci < 64; w++) {
        size_t wlen = strlen(words[w]);

        if (wlen <= PS_LENGTH) {
            /* Word fits in a single chunk - pad with spaces */
            memset(g_ps_scroll.chunks[ci], ' ', PS_LENGTH);
            memcpy(g_ps_scroll.chunks[ci], words[w], wlen);
            g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
            ci++;
        } else {
            /* Word too long - split with dashes */
            size_t wpos = 0;
            while (wpos < wlen && ci < 64) {
                size_t rem = wlen - wpos;
                if (rem <= PS_LENGTH) {
                    memset(g_ps_scroll.chunks[ci], ' ', PS_LENGTH);
                    memcpy(g_ps_scroll.chunks[ci], words[w] + wpos, rem);
                    g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
                    ci++;
                    break;
                } else {
                    memcpy(g_ps_scroll.chunks[ci], words[w] + wpos, PS_LENGTH - 1);
                    g_ps_scroll.chunks[ci][PS_LENGTH - 1] = '-';
                    g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
                    wpos += PS_LENGTH - 1;
                    ci++;
                }
            }
        }
    }

    g_ps_scroll.num_chunks = ci;
    g_ps_scroll.current_chunk = 0;
    g_ps_scroll.last_advance_tick = GetTickCount();
}

/* Advance PS scroll if it's time */
static void ps_scroll_tick(void) {
    if (g_ps_scroll.num_chunks <= 1) return;
    if (!g_engine_running) return;

    DWORD now = GetTickCount();
    if ((now - g_ps_scroll.last_advance_tick) >= PS_SCROLL_INTERVAL_MS) {
        g_ps_scroll.current_chunk =
            (g_ps_scroll.current_chunk + 1) % g_ps_scroll.num_chunks;
        set_rds_ps(xlat((unsigned char *)
            g_ps_scroll.chunks[g_ps_scroll.current_chunk]));
        g_ps_scroll.last_advance_tick = now;
    }
}

/* -----------------------------------------------------------------------
 * File Watch Helpers
 * ----------------------------------------------------------------------- */

static BOOL get_file_write_time(const char *path, FILETIME *ft) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = GetFileTime(h, NULL, NULL, ft);
    CloseHandle(h);
    return ok;
}

static BOOL file_time_changed(FILETIME *a, FILETIME *b) {
    return (a->dwHighDateTime != b->dwHighDateTime ||
            a->dwLowDateTime != b->dwLowDateTime);
}

static char *read_file_text(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0 || sz > 65536) { fclose(fp); return NULL; }
    rewind(fp);

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t rd = fread(buf, 1, sz, fp);
    buf[rd] = '\0';
    fclose(fp);

    /* Trim trailing whitespace */
    while (rd > 0 && (buf[rd-1] == '\n' || buf[rd-1] == '\r' ||
           buf[rd-1] == ' ' || buf[rd-1] == '\t'))
        buf[--rd] = '\0';

    return buf;
}

/* -----------------------------------------------------------------------
 * File Watch: RT
 * ----------------------------------------------------------------------- */

static void process_rt_file(void) {
    char *text = read_file_text(g_rt_file.path);
    if (!text) return;

    if (text[0]) {
        set_rds_rt(xlat((unsigned char *)text));
        log_msg("[RT File] Updated: \"%s\"\r\n", text);
    }
    free(text);
}

/* -----------------------------------------------------------------------
 * File Watch: PS (auto-chunk)
 * ----------------------------------------------------------------------- */

static void process_ps_file(void) {
    char *text = read_file_text(g_ps_file.path);
    if (!text) return;

    if (text[0]) {
        strncpy(g_ps_scroll.full_text, text, sizeof(g_ps_scroll.full_text) - 1);
        g_ps_scroll.full_text[sizeof(g_ps_scroll.full_text) - 1] = '\0';
        ps_chunk_text(text);

        if (g_ps_scroll.num_chunks > 0) {
            set_rds_ps(xlat((unsigned char *)g_ps_scroll.chunks[0]));
            log_msg("[PS File] Loaded %d chunk(s) from: \"%s\"\r\n",
                    g_ps_scroll.num_chunks, text);
            for (int i = 0; i < g_ps_scroll.num_chunks; i++) {
                log_msg("  Chunk %d: \"%.8s\"\r\n", i + 1, g_ps_scroll.chunks[i]);
            }
        }
    }
    free(text);
}

/* -----------------------------------------------------------------------
 * File Watch: RT+ (artist || title format)
 *
 * File format: "Artist Name || Song Title"
 * The || separator delimits the two RT+ content fields.
 * Only configures RT+ tags - does NOT modify RT.
 * Tag positions are found by searching the current RT text for the
 * artist and title substrings (case-insensitive).
 * ----------------------------------------------------------------------- */

static void process_rtp_file(void) {
    char *text = read_file_text(g_rtp_file.path);
    if (!text) return;
    if (!text[0]) { free(text); return; }

    /* Find the || separator */
    char *sep = strstr(text, "||");
    if (!sep) {
        log_msg("[RT+ File] Warning: no '||' separator found. "
                "Format: artist || title\r\n");
        free(text);
        return;
    }

    /* Extract artist and title */
    char artist[RT_LENGTH];
    char title[RT_LENGTH];

    /* Artist (trim trailing spaces before ||) */
    size_t artist_raw_len = sep - text;
    while (artist_raw_len > 0 && text[artist_raw_len - 1] == ' ')
        artist_raw_len--;
    if (artist_raw_len >= RT_LENGTH) artist_raw_len = RT_LENGTH - 1;
    memcpy(artist, text, artist_raw_len);
    artist[artist_raw_len] = '\0';

    /* Title (skip || and leading spaces) */
    char *title_start = sep + 2;
    while (*title_start == ' ') title_start++;
    strncpy(title, title_start, RT_LENGTH - 1);
    title[RT_LENGTH - 1] = '\0';

    /* Read the current RT text from the encoder */
    struct rds_params_t p;
    get_rds_params_copy(&p);
    char current_rt[RT_LENGTH + 1];
    memcpy(current_rt, p.rt, RT_LENGTH);
    current_rt[RT_LENGTH] = '\0';
    /* Trim trailing spaces */
    for (int i = RT_LENGTH - 1; i >= 0 && current_rt[i] == ' '; i--)
        current_rt[i] = '\0';

    /* Search for artist and title in the current RT (case-insensitive) */
    char *artist_pos = NULL;
    char *title_pos = NULL;
    {
        /* Case-insensitive substring search */
        size_t rt_len = strlen(current_rt);
        size_t a_len = strlen(artist);
        size_t t_len = strlen(title);

        if (a_len > 0 && a_len <= rt_len) {
            for (size_t i = 0; i <= rt_len - a_len; i++) {
                if (strncasecmp(current_rt + i, artist, a_len) == 0) {
                    artist_pos = current_rt + i;
                    break;
                }
            }
        }
        if (t_len > 0 && t_len <= rt_len) {
            for (size_t i = 0; i <= rt_len - t_len; i++) {
                if (strncasecmp(current_rt + i, title, t_len) == 0) {
                    title_pos = current_rt + i;
                    break;
                }
            }
        }
    }

    if (!artist_pos && !title_pos) {
        log_msg("[RT+ File] Warning: neither artist \"%s\" nor title \"%s\" "
                "found in current RT: \"%s\"\r\n", artist, title, current_rt);
        free(text);
        return;
    }

    /* Calculate RT+ tag positions from located substrings */
    uint8_t artist_start_pos = artist_pos ? (uint8_t)(artist_pos - current_rt) : 0;
    uint8_t artist_tag_len = artist_pos ? (uint8_t)(strlen(artist) - 1) : 0;
    uint8_t title_start_pos = title_pos ? (uint8_t)(title_pos - current_rt) : 0;
    uint8_t title_tag_len = title_pos ? (uint8_t)(strlen(title) - 1) : 0;
    uint8_t artist_type = artist_pos ? 4 : 0; /* ITEM.ARTIST or DUMMY */
    uint8_t title_type = title_pos ? 1 : 0;   /* ITEM.TITLE or DUMMY */

    uint8_t tags[6] = {
        artist_type, artist_start_pos, artist_tag_len,
        title_type,  title_start_pos,  title_tag_len
    };
    set_rds_rtplus_tags(tags);
    set_rds_rtplus_flags(3); /* running=1, toggle=1 */

    log_msg("[RT+ File] Artist: \"%s\" @ pos %u, Title: \"%s\" @ pos %u\r\n",
            artist, artist_start_pos, title, title_start_pos);
    log_msg("[RT+ File] RT+ tags set (RT unchanged: \"%s\")\r\n", current_rt);

    free(text);
}

/* -----------------------------------------------------------------------
 * File Watch: PT (Program Type)
 * ----------------------------------------------------------------------- */

static void process_pt_file(void) {
    char *text = read_file_text(g_pt_file.path);
    if (!text) return;

    if (text[0]) {
        uint8_t pty;
        if (text[0] >= 'A') {
            pty = get_pty_code(text);
        } else {
            pty = (uint8_t)strtoul(text, NULL, 10);
        }
        set_rds_pty(pty);
        log_msg("[PT File] PTY set to %u\r\n", pty);
    }
    free(text);
}

/* -----------------------------------------------------------------------
 * File Watch Timer Callback
 * ----------------------------------------------------------------------- */

static void check_file_watches(void) {
    FILETIME ft;

    if (g_rt_file.active && g_rt_file.path[0]) {
        if (get_file_write_time(g_rt_file.path, &ft)) {
            if (file_time_changed(&ft, &g_rt_file.last_write)) {
                g_rt_file.last_write = ft;
                if (g_engine_running) process_rt_file();
            }
        }
    }

    if (g_ps_file.active && g_ps_file.path[0]) {
        if (get_file_write_time(g_ps_file.path, &ft)) {
            if (file_time_changed(&ft, &g_ps_file.last_write)) {
                g_ps_file.last_write = ft;
                if (g_engine_running) process_ps_file();
            }
        }
    }

    if (g_rtp_file.active && g_rtp_file.path[0]) {
        if (get_file_write_time(g_rtp_file.path, &ft)) {
            if (file_time_changed(&ft, &g_rtp_file.last_write)) {
                g_rtp_file.last_write = ft;
                if (g_engine_running) process_rtp_file();
            }
        }
    }

    if (g_pt_file.active && g_pt_file.path[0]) {
        if (get_file_write_time(g_pt_file.path, &ft)) {
            if (file_time_changed(&ft, &g_pt_file.last_write)) {
                g_pt_file.last_write = ft;
                if (g_engine_running) process_pt_file();
            }
        }
    }

    /* PS scrolling */
    ps_scroll_tick();
}

/* -----------------------------------------------------------------------
 * RDS Monitor Update
 * ----------------------------------------------------------------------- */

static void update_monitor(void) {
    if (!g_engine_running) return;

    struct rds_params_t p;
    struct rds_rtplus_info_t rtp;
    char buf[256];

    get_rds_params_copy(&p);
    get_rds_rtplus_info(&rtp);

    snprintf(buf, sizeof(buf), "%04X", p.pi);
    SetDlgItemTextA(g_hwnd, IDC_MON_PI, buf);

    /* PS */
    {
        char ps_display[PS_LENGTH + 1];
        memcpy(ps_display, p.ps, PS_LENGTH);
        ps_display[PS_LENGTH] = '\0';
        SetDlgItemTextA(g_hwnd, IDC_MON_PS, ps_display);
    }

    /* RT */
    {
        char rt_display[RT_LENGTH + 1];
        memcpy(rt_display, p.rt, RT_LENGTH);
        rt_display[RT_LENGTH] = '\0';
        for (int i = RT_LENGTH - 1; i >= 0 && rt_display[i] == ' '; i--)
            rt_display[i] = '\0';
        SetDlgItemTextA(g_hwnd, IDC_MON_RT, rt_display);
    }

    snprintf(buf, sizeof(buf), "%u", p.pty);
    SetDlgItemTextA(g_hwnd, IDC_MON_PTY, buf);

    /* PTYN */
    {
        char ptyn_display[PTYN_LENGTH + 1];
        memcpy(ptyn_display, p.ptyn, PTYN_LENGTH);
        ptyn_display[PTYN_LENGTH] = '\0';
        for (int i = PTYN_LENGTH - 1; i >= 0 && ptyn_display[i] == ' '; i--)
            ptyn_display[i] = '\0';
        SetDlgItemTextA(g_hwnd, IDC_MON_PTYN,
                        ptyn_display[0] ? ptyn_display : "(none)");
    }

    SetDlgItemTextA(g_hwnd, IDC_MON_TP, p.tp ? "ON" : "OFF");
    SetDlgItemTextA(g_hwnd, IDC_MON_TA, p.ta ? "ON" : "OFF");
    SetDlgItemTextA(g_hwnd, IDC_MON_MS, p.ms ? "Music" : "Speech");

    /* LPS */
    {
        char lps_display[LPS_LENGTH + 1];
        memcpy(lps_display, p.lps, LPS_LENGTH);
        lps_display[LPS_LENGTH] = '\0';
        SetDlgItemTextA(g_hwnd, IDC_MON_LPS,
                        lps_display[0] ? lps_display : "(none)");
    }

    /* eRT */
    {
        char ert_display[ERT_LENGTH + 1];
        memcpy(ert_display, p.ert, ERT_LENGTH);
        ert_display[ERT_LENGTH] = '\0';
        for (int i = ERT_LENGTH - 1; i >= 0 && ert_display[i] == ' '; i--)
            ert_display[i] = '\0';
        SetDlgItemTextA(g_hwnd, IDC_MON_ERT,
                        ert_display[0] ? ert_display : "(none)");
    }

    /* RT+ Tag 1 & 2 */
    if (rtp.running) {
        snprintf(buf, sizeof(buf), "%s (start=%u, len=%u)",
                 get_rtp_tag_name(rtp.type[0]), rtp.start[0], rtp.len[0]);
        SetDlgItemTextA(g_hwnd, IDC_MON_RTP1, buf);
        snprintf(buf, sizeof(buf), "%s (start=%u, len=%u)",
                 get_rtp_tag_name(rtp.type[1]), rtp.start[1], rtp.len[1]);
        SetDlgItemTextA(g_hwnd, IDC_MON_RTP2, buf);
    } else {
        SetDlgItemTextA(g_hwnd, IDC_MON_RTP1, "(inactive)");
        SetDlgItemTextA(g_hwnd, IDC_MON_RTP2, "(inactive)");
    }

    /* AF */
    {
        char *af_str = show_af_list(p.af);
        SetDlgItemTextA(g_hwnd, IDC_MON_AF,
                        (af_str && af_str[0]) ? af_str : "(none)");
    }
}

/* -----------------------------------------------------------------------
 * RDS Engine Thread
 * ----------------------------------------------------------------------- */

static DWORD WINAPI engine_thread_proc(LPVOID param) {
    (void)param;

    float *mpx_buffer = NULL;
    float *out_buffer = NULL;
    char *dev_out = NULL;

    SRC_STATE *src_state = NULL;
    SRC_DATA src_data;

    ao_device *device = NULL;
    ao_sample_format format;
    ao_option *ao_opts = NULL;

    size_t frames;
    unsigned long loop_count = 0;

    mpx_buffer = malloc(NUM_MPX_FRAMES_IN * 2 * sizeof(float));
    out_buffer = malloc(NUM_MPX_FRAMES_OUT * 2 * sizeof(float));
    dev_out = malloc(NUM_MPX_FRAMES_OUT * 2 * sizeof(int16_t) * sizeof(char));

    if (!mpx_buffer || !out_buffer || !dev_out) {
        fprintf(stderr, "Error: failed to allocate audio buffers.\n");
        goto engine_exit;
    }

    fm_mpx_init(MPX_SAMPLE_RATE);
    set_output_volume(g_volume);
    fprintf(stderr, "Baseband generator initialized at %d Hz.\n", MPX_SAMPLE_RATE);

    /* Initialize RDS encoder with GUI values */
    {
        struct rds_params_t rds_params = {
            .ps = "MiniRDS",
            .rt = "MiniRDS: Software RDS encoder",
            .pi = 0x1000
        };

        char buf[256];
        GetDlgItemTextA(g_hwnd, IDC_PI_EDIT, buf, sizeof(buf));
        if (buf[0]) {
#ifdef RBDS
            if (buf[0] == 'K' || buf[0] == 'W' || buf[0] == 'k' || buf[0] == 'w')
                rds_params.pi = callsign2pi((unsigned char *)buf);
            else
#endif
                rds_params.pi = (uint16_t)strtoul(buf, NULL, 16);
        }
        GetDlgItemTextA(g_hwnd, IDC_PS_EDIT, buf, sizeof(buf));
        if (buf[0]) memcpy(rds_params.ps, xlat((unsigned char *)buf), PS_LENGTH);

        GetDlgItemTextA(g_hwnd, IDC_RT_EDIT, buf, sizeof(buf));
        if (buf[0]) memcpy(rds_params.rt, xlat((unsigned char *)buf), RT_LENGTH);

        GetDlgItemTextA(g_hwnd, IDC_PTY_EDIT, buf, sizeof(buf));
        if (buf[0]) rds_params.pty = (uint8_t)strtoul(buf, NULL, 10);

        rds_params.tp = (IsDlgButtonChecked(g_hwnd, IDC_TP_CHECK) == BST_CHECKED) ? 1 : 0;

        GetDlgItemTextA(g_hwnd, IDC_PTYN_EDIT, buf, sizeof(buf));
        if (buf[0]) memcpy(rds_params.ptyn, xlat((unsigned char *)buf), PTYN_LENGTH);

        GetDlgItemTextA(g_hwnd, IDC_AF_EDIT, buf, sizeof(buf));
        if (buf[0]) {
            char *tok = strtok(buf, " ,;");
            while (tok) {
                add_rds_af(&rds_params.af, strtof(tok, NULL));
                tok = strtok(NULL, " ,;");
            }
        }

        init_rds_encoder(rds_params);
        fprintf(stderr, "RDS encoder initialized (PI=%04X, PS=\"%.8s\").\n",
                rds_params.pi, rds_params.ps);
    }

    if (IsDlgButtonChecked(g_hwnd, IDC_TA_CHECK) == BST_CHECKED)
        set_rds_ta(1);
    if (IsDlgButtonChecked(g_hwnd, IDC_MS_CHECK) == BST_CHECKED)
        set_rds_ms(1);

    {
        char buf[256];
        GetDlgItemTextA(g_hwnd, IDC_LPS_EDIT, buf, sizeof(buf));
        if (buf[0]) set_rds_lps((unsigned char *)buf);

        GetDlgItemTextA(g_hwnd, IDC_ERT_EDIT, buf, sizeof(buf));
        if (buf[0]) set_rds_ert((unsigned char *)buf);
    }

    /* Process active file watches immediately */
    if (g_rt_file.active && g_rt_file.path[0]) {
        get_file_write_time(g_rt_file.path, &g_rt_file.last_write);
        process_rt_file();
    }
    if (g_ps_file.active && g_ps_file.path[0]) {
        get_file_write_time(g_ps_file.path, &g_ps_file.last_write);
        process_ps_file();
    }
    if (g_rtp_file.active && g_rtp_file.path[0]) {
        get_file_write_time(g_rtp_file.path, &g_rtp_file.last_write);
        process_rtp_file();
    }
    if (g_pt_file.active && g_pt_file.path[0]) {
        get_file_write_time(g_pt_file.path, &g_pt_file.last_write);
        process_pt_file();
    }

    /* Setup audio output */
    memset(&format, 0, sizeof(format));
    format.channels = 2;
    format.bits = 16;
    format.rate = OUTPUT_SAMPLE_RATE;
    format.byte_format = AO_FMT_LITTLE;

    ao_initialize();

    {
        int driver_id = ao_default_driver_id();
        ao_info *info;

        if (driver_id < 0) {
            fprintf(stderr, "Error: no usable audio driver found.\n");
            goto engine_cleanup;
        }

        info = ao_driver_info(driver_id);
        if (info)
            fprintf(stderr, "Audio driver: %s (%s)\n", info->name, info->short_name);

        if (g_selected_device >= 0) {
            char dev_id_str[16];
            snprintf(dev_id_str, sizeof(dev_id_str), "%d", g_selected_device);
            ao_append_option(&ao_opts, "id", dev_id_str);
            fprintf(stderr, "Using audio device ID: %d\n", g_selected_device);
        }

        fprintf(stderr, "Opening audio: %d-bit, %dch, %d Hz...\n",
                format.bits, format.channels, format.rate);

        device = ao_open_live(driver_id, &format, ao_opts);
        if (ao_opts) ao_free_options(ao_opts);

        if (!device) {
            fprintf(stderr, "Error: cannot open audio device.\n");
            goto engine_cleanup;
        }

        fprintf(stderr, "Audio device opened successfully.\n");
    }

    /* Setup resampler */
    memset(&src_data, 0, sizeof(SRC_DATA));
    src_data.input_frames = NUM_MPX_FRAMES_IN;
    src_data.output_frames = NUM_MPX_FRAMES_OUT;
    src_data.src_ratio = (double)OUTPUT_SAMPLE_RATE / (double)MPX_SAMPLE_RATE;
    src_data.data_in = mpx_buffer;
    src_data.data_out = out_buffer;

    if (resampler_init(&src_state, 2) < 0) {
        fprintf(stderr, "Error: could not create resampler.\n");
        goto engine_cleanup;
    }

    fprintf(stderr, "Resampler initialized (ratio=%.6f).\n", src_data.src_ratio);
    fprintf(stderr, "RDS output started.\n");

    /* Main generation loop */
    while (!g_stop_engine) {
        fm_rds_get_frames(mpx_buffer, NUM_MPX_FRAMES_IN);

        if (resample(src_state, src_data, &frames) < 0) {
            fprintf(stderr, "Error: resampler failed at iteration %lu.\n", loop_count);
            break;
        }
        if (frames == 0) continue;

        float2char2channel(out_buffer, dev_out, frames);

        if (!ao_play(device, dev_out, frames * 2 * sizeof(int16_t))) {
            fprintf(stderr, "Error: ao_play failed at iteration %lu.\n", loop_count);
            break;
        }
        loop_count++;
    }

    fprintf(stderr, "Engine stopped after %lu iterations.\n", loop_count);

    if (src_state) resampler_exit(src_state);
    if (device) ao_close(device);

engine_cleanup:
    ao_shutdown();
    fm_mpx_exit();
    exit_rds_encoder();
    fprintf(stderr, "Engine cleanup complete.\n");

engine_exit:
    free(mpx_buffer);
    free(out_buffer);
    free(dev_out);

    InterlockedExchange(&g_engine_running, 0);
    PostMessageA(g_hwnd, WM_APP + 1, 0, 0);

    return 0;
}

/* -----------------------------------------------------------------------
 * Engine Control
 * ----------------------------------------------------------------------- */

static void start_engine(void) {
    if (InterlockedCompareExchange(&g_engine_running, 1, 0) != 0) {
        log_msg("Engine is already running.\r\n");
        return;
    }

    g_stop_engine = 0;

    HWND combo = GetDlgItem(g_hwnd, IDC_DEVICE_COMBO);
    int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
    g_selected_device = (sel <= 0) ? -1 : (int)g_audio_devices[sel - 1].id;

    HWND slider = GetDlgItem(g_hwnd, IDC_VOLUME_SLIDER);
    g_volume = (float)SendMessageA(slider, TBM_GETPOS, 0, 0);

    /* Capture file watch state from GUI */
    GetDlgItemTextA(g_hwnd, IDC_RT_FILE_EDIT, g_rt_file.path, MAX_PATH);
    g_rt_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_RT_FILE_ACTIVE) == BST_CHECKED);
    memset(&g_rt_file.last_write, 0, sizeof(FILETIME));

    GetDlgItemTextA(g_hwnd, IDC_PS_FILE_EDIT, g_ps_file.path, MAX_PATH);
    g_ps_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_PS_FILE_ACTIVE) == BST_CHECKED);
    memset(&g_ps_file.last_write, 0, sizeof(FILETIME));

    GetDlgItemTextA(g_hwnd, IDC_RTP_FILE_EDIT, g_rtp_file.path, MAX_PATH);
    g_rtp_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_RTP_FILE_ACTIVE) == BST_CHECKED);
    memset(&g_rtp_file.last_write, 0, sizeof(FILETIME));

    g_ps_scroll.num_chunks = 0;
    g_ps_scroll.current_chunk = 0;

    g_engine_thread = CreateThread(NULL, 0, engine_thread_proc, NULL, 0, NULL);
    if (!g_engine_thread) {
        log_msg("Error: could not create engine thread.\r\n");
        InterlockedExchange(&g_engine_running, 0);
        return;
    }

    SetDlgItemTextA(g_hwnd, IDC_STATUS_LABEL, "Status: Running");
    EnableWindow(GetDlgItem(g_hwnd, IDC_START_BTN), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP_BTN), TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_DEVICE_COMBO), FALSE);
    log_msg("Starting RDS engine...\r\n");
}

static void stop_engine(void) {
    if (!g_engine_running) {
        log_msg("Engine is not running.\r\n");
        return;
    }

    log_msg("Stopping RDS engine...\r\n");
    InterlockedExchange((volatile LONG *)&g_stop_engine, 1);

    if (g_engine_thread) {
        WaitForSingleObject(g_engine_thread, 5000);
        CloseHandle(g_engine_thread);
        g_engine_thread = NULL;
    }

    SetDlgItemTextA(g_hwnd, IDC_STATUS_LABEL, "Status: Stopped");
    EnableWindow(GetDlgItem(g_hwnd, IDC_START_BTN), TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP_BTN), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_DEVICE_COMBO), TRUE);
}

/* -----------------------------------------------------------------------
 * Apply Settings (live update)
 * ----------------------------------------------------------------------- */

static void apply_settings(void) {
    char buf[256];

    GetDlgItemTextA(g_hwnd, IDC_PI_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        uint16_t pi;
#ifdef RBDS
        if (buf[0] == 'K' || buf[0] == 'W' || buf[0] == 'k' || buf[0] == 'w')
            pi = callsign2pi((unsigned char *)buf);
        else
#endif
            pi = (uint16_t)strtoul(buf, NULL, 16);
        set_rds_pi(pi);
        log_msg("PI set to %04X\r\n", pi);
    }

    GetDlgItemTextA(g_hwnd, IDC_PS_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        set_rds_ps(xlat((unsigned char *)buf));
        log_msg("PS set to \"%.8s\"\r\n", buf);
    }

    GetDlgItemTextA(g_hwnd, IDC_RT_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        set_rds_rt(xlat((unsigned char *)buf));
        log_msg("RT set to \"%s\"\r\n", buf);
    }

    GetDlgItemTextA(g_hwnd, IDC_PTY_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        uint8_t pty = (uint8_t)strtoul(buf, NULL, 10);
        set_rds_pty(pty);
        log_msg("PTY set to %u\r\n", pty);
    }

    GetDlgItemTextA(g_hwnd, IDC_PTYN_EDIT, buf, sizeof(buf));
    if (buf[0] && buf[0] != '-') {
        set_rds_ptyn(xlat((unsigned char *)buf));
        log_msg("PTYN set to \"%.8s\"\r\n", buf);
    } else if (buf[0] == '-') {
        unsigned char empty = 0;
        set_rds_ptyn(&empty);
        log_msg("PTYN cleared\r\n");
    }

    set_rds_tp((IsDlgButtonChecked(g_hwnd, IDC_TP_CHECK) == BST_CHECKED) ? 1 : 0);
    set_rds_ta((IsDlgButtonChecked(g_hwnd, IDC_TA_CHECK) == BST_CHECKED) ? 1 : 0);
    set_rds_ms((IsDlgButtonChecked(g_hwnd, IDC_MS_CHECK) == BST_CHECKED) ? 1 : 0);

    GetDlgItemTextA(g_hwnd, IDC_LPS_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        if (buf[0] == '-') buf[0] = 0;
        set_rds_lps((unsigned char *)buf);
    }

    GetDlgItemTextA(g_hwnd, IDC_ERT_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        if (buf[0] == '-') buf[0] = 0;
        set_rds_ert((unsigned char *)buf);
    }

    HWND slider = GetDlgItem(g_hwnd, IDC_VOLUME_SLIDER);
    float vol = (float)SendMessageA(slider, TBM_GETPOS, 0, 0);
    set_output_volume(vol);

    /* Update file watch state live */
    g_rt_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_RT_FILE_ACTIVE) == BST_CHECKED);
    GetDlgItemTextA(g_hwnd, IDC_RT_FILE_EDIT, g_rt_file.path, MAX_PATH);

    g_ps_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_PS_FILE_ACTIVE) == BST_CHECKED);
    GetDlgItemTextA(g_hwnd, IDC_PS_FILE_EDIT, g_ps_file.path, MAX_PATH);

    g_rtp_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_RTP_FILE_ACTIVE) == BST_CHECKED);
    GetDlgItemTextA(g_hwnd, IDC_RTP_FILE_EDIT, g_rtp_file.path, MAX_PATH);

    g_pt_file.active =
        (IsDlgButtonChecked(g_hwnd, IDC_PT_FILE_ACTIVE) == BST_CHECKED);
    GetDlgItemTextA(g_hwnd, IDC_PT_FILE_EDIT, g_pt_file.path, MAX_PATH);

    log_msg("Settings applied.\r\n");
}

/* -----------------------------------------------------------------------
 * Command File
 * ----------------------------------------------------------------------- */

static void browse_for_file(int edit_id) {
    OPENFILENAMEA ofn;
    char file_path[MAX_PATH] = "";

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn))
        SetDlgItemTextA(g_hwnd, edit_id, file_path);
}

static void execute_command_file(void) {
    char file_path[MAX_PATH];
    FILE *fp;
    char line[CMD_BUFFER_SIZE];
    int count = 0;

    GetDlgItemTextA(g_hwnd, IDC_FILE_EDIT, file_path, MAX_PATH);
    if (!file_path[0]) {
        log_msg("No command file specified.\r\n");
        return;
    }

    fp = fopen(file_path, "r");
    if (!fp) {
        log_msg("Error: cannot open file: %s\r\n", file_path);
        return;
    }

    log_msg("Executing commands from: %s\r\n", file_path);

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        log_msg("  CMD: %s\r\n", line);
        process_ascii_cmd((unsigned char *)line);
        count++;
    }

    fclose(fp);
    log_msg("Executed %d commands.\r\n", count);
}

/* -----------------------------------------------------------------------
 * GUI Control Creation Helpers
 * ----------------------------------------------------------------------- */

static HWND create_label(HWND parent, const char *text,
                         int x, int y, int w, int h) {
    return CreateWindowA("STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, NULL,
        GetModuleHandle(NULL), NULL);
}

static HWND create_label_id(HWND parent, int id, const char *text,
                            int x, int y, int w, int h) {
    return CreateWindowA("STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, (HMENU)(intptr_t)id,
        GetModuleHandle(NULL), NULL);
}

static HWND create_edit(HWND parent, int id, const char *text,
                        int x, int y, int w, int h, DWORD extra_style) {
    return CreateWindowA("EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
        x, y, w, h, parent, (HMENU)(intptr_t)id,
        GetModuleHandle(NULL), NULL);
}

static HWND create_button(HWND parent, int id, const char *text,
                          int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(intptr_t)id,
        GetModuleHandle(NULL), NULL);
}

static HWND create_checkbox(HWND parent, int id, const char *text,
                            int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, h, parent, (HMENU)(intptr_t)id,
        GetModuleHandle(NULL), NULL);
}

static HWND create_groupbox(HWND parent, const char *text,
                            int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, parent, NULL,
        GetModuleHandle(NULL), NULL);
}

static BOOL CALLBACK SetFont_EnumProc(HWND hwnd, LPARAM lParam);

/* Create a "Label: Value" pair for the monitor section */
static int create_monitor_row(HWND hwnd, const char *label, int id,
                              int x, int y, int label_w, int val_w) {
    HWND lbl = create_label(hwnd, label, x, y, label_w, 16);
    SendMessageA(lbl, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
    HWND val = create_label_id(hwnd, id, "", x + label_w + 4, y, val_w, 16);
    SendMessageA(val, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    return 18;
}

/* -----------------------------------------------------------------------
 * Create All Controls
 * ----------------------------------------------------------------------- */

static void create_all_controls(HWND hwnd) {
    HWND ctrl;
    HINSTANCE hinst = GetModuleHandle(NULL);
    int lx = 10, gw = 635;
    int y;

    /* =================================================================
     * RDS Settings Group
     * ================================================================= */
    create_groupbox(hwnd, "RDS Settings", lx, 5, gw, 220);

    /* Row 1: PI, PS, PTY, PTYN */
    create_label(hwnd, "PI:", 20, 28, 20, 18);
    create_edit(hwnd, IDC_PI_EDIT, "1000", 42, 25, 70, 22, 0);

    create_label(hwnd, "PS:", 125, 28, 20, 18);
    create_edit(hwnd, IDC_PS_EDIT, "MiniRDS", 148, 25, 130, 22, 0);

    create_label(hwnd, "PTY:", 295, 28, 28, 18);
    create_edit(hwnd, IDC_PTY_EDIT, "0", 325, 25, 35, 22, ES_NUMBER);

    create_label(hwnd, "PTYN:", 380, 28, 35, 18);
    create_edit(hwnd, IDC_PTYN_EDIT, "", 418, 25, 130, 22, 0);

    /* Row 2: RT */
    create_label(hwnd, "RT:", 20, 58, 20, 18);
    create_edit(hwnd, IDC_RT_EDIT, "MiniRDS: Software RDS encoder", 42, 55, 590, 22, 0);

    /* Row 3: TP, TA, MS */
    create_checkbox(hwnd, IDC_TP_CHECK, "TP", 20, 85, 45, 22);
    create_checkbox(hwnd, IDC_TA_CHECK, "TA", 75, 85, 45, 22);
    create_checkbox(hwnd, IDC_MS_CHECK, "MS", 130, 85, 45, 22);

    /* Row 4: AF */
    create_label(hwnd, "AF:", 20, 113, 20, 18);
    create_edit(hwnd, IDC_AF_EDIT, "", 42, 110, 590, 22, 0);

    /* Row 5: LPS */
    create_label(hwnd, "LPS:", 20, 143, 28, 18);
    create_edit(hwnd, IDC_LPS_EDIT, "", 50, 140, 582, 22, 0);

    /* Row 6: eRT */
    create_label(hwnd, "eRT:", 20, 173, 28, 18);
    create_edit(hwnd, IDC_ERT_EDIT, "", 50, 170, 582, 22, 0);

    /* Apply button */
    create_button(hwnd, IDC_APPLY_BTN, "Apply Settings", 20, 196, 120, 22);
    create_label(hwnd, "(Updates take effect immediately while running)",
                 148, 199, 350, 16);

    /* =================================================================
     * File Watch Inputs Group
     * ================================================================= */
    y = 230;
    create_groupbox(hwnd, "File Watch Inputs (auto-reload on change)", lx, y, gw, 138);
    y += 20;

    /* RT File */
    create_label(hwnd, "RT File:", 20, y + 3, 50, 16);
    create_edit(hwnd, IDC_RT_FILE_EDIT, "", 72, y, 382, 22, 0);
    create_button(hwnd, IDC_RT_FILE_BROWSE, "...", 458, y, 28, 22);
    create_checkbox(hwnd, IDC_RT_FILE_ACTIVE, "Watch", 492, y + 1, 58, 20);
    y += 28;

    /* PS File */
    create_label(hwnd, "PS File:", 20, y + 3, 50, 16);
    create_edit(hwnd, IDC_PS_FILE_EDIT, "", 72, y, 382, 22, 0);
    create_button(hwnd, IDC_PS_FILE_BROWSE, "...", 458, y, 28, 22);
    create_checkbox(hwnd, IDC_PS_FILE_ACTIVE, "Watch", 492, y + 1, 58, 20);
    create_label(hwnd, "(auto-chunks)", 555, y + 3, 82, 16);
    y += 28;

    /* RT+ File */
    create_label(hwnd, "RT+ File:", 20, y + 3, 50, 16);
    create_edit(hwnd, IDC_RTP_FILE_EDIT, "", 72, y, 382, 22, 0);
    create_button(hwnd, IDC_RTP_FILE_BROWSE, "...", 458, y, 28, 22);
    create_checkbox(hwnd, IDC_RTP_FILE_ACTIVE, "Watch", 492, y + 1, 58, 20);
    create_label(hwnd, "(artist || title)", 555, y + 3, 82, 16);
    y += 28;

    /* PT File */
    create_label(hwnd, "PT File:", 20, y + 3, 50, 16);
    create_edit(hwnd, IDC_PT_FILE_EDIT, "", 72, y, 382, 22, 0);
    create_button(hwnd, IDC_PT_FILE_BROWSE, "...", 458, y, 28, 22);
    create_checkbox(hwnd, IDC_PT_FILE_ACTIVE, "Watch", 492, y + 1, 58, 20);
    create_label(hwnd, "(PTY number)", 555, y + 3, 82, 16);

    /* =================================================================
     * Audio Output Group
     * ================================================================= */
    y = 373;
    create_groupbox(hwnd, "Audio Output", lx, y, gw, 72);

    create_label(hwnd, "Device:", 20, y + 20, 45, 18);
    ctrl = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        68, y + 17, 460, 200, hwnd, (HMENU)(intptr_t)IDC_DEVICE_COMBO,
        hinst, NULL);
    enumerate_audio_devices(ctrl);

    create_label(hwnd, "Volume:", 20, y + 48, 45, 18);
    ctrl = CreateWindowA(TRACKBAR_CLASSA, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        68, y + 45, 440, 25, hwnd, (HMENU)(intptr_t)IDC_VOLUME_SLIDER,
        hinst, NULL);
    SendMessageA(ctrl, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageA(ctrl, TBM_SETPOS, TRUE, 50);
    SendMessageA(ctrl, TBM_SETTICFREQ, 10, 0);
    create_label_id(hwnd, IDC_VOLUME_LABEL, "50%", 515, y + 48, 40, 18);

    /* =================================================================
     * Control Group
     * ================================================================= */
    y = 450;
    create_groupbox(hwnd, "Control", lx, y, gw, 78);

    create_button(hwnd, IDC_START_BTN, "Start", 20, y + 20, 80, 26);
    ctrl = create_button(hwnd, IDC_STOP_BTN, "Stop", 108, y + 20, 80, 26);
    EnableWindow(ctrl, FALSE);
    create_label_id(hwnd, IDC_STATUS_LABEL, "Status: Stopped", 200, y + 26, 200, 18);

    create_label(hwnd, "Commands:", 20, y + 53, 62, 18);
    create_edit(hwnd, IDC_FILE_EDIT, "", 85, y + 50, 335, 22, 0);
    create_button(hwnd, IDC_BROWSE_BTN, "Browse...", 425, y + 50, 68, 22);
    create_button(hwnd, IDC_EXEC_BTN, "Execute", 498, y + 50, 68, 22);

    /* =================================================================
     * Live RDS Monitor Group
     * ================================================================= */
    y = 533;
    create_groupbox(hwnd, "Live RDS Monitor", lx, y, gw, 215);
    y += 18;

    int lw = 42; /* label width for monitor row headers */

    /* Row: PI + TP/TA/MS in two columns */
    int col2_x = 320, col2_lw = 32;

    y += create_monitor_row(hwnd, "PI:", IDC_MON_PI,
                            20, y, lw, 80);
    create_monitor_row(hwnd, "TP:", IDC_MON_TP,
                       col2_x, y - 18, col2_lw, 50);

    y += create_monitor_row(hwnd, "PS:", IDC_MON_PS,
                            20, y, lw, 250);
    create_monitor_row(hwnd, "TA:", IDC_MON_TA,
                       col2_x, y - 18, col2_lw, 50);

    y += create_monitor_row(hwnd, "PTY:", IDC_MON_PTY,
                            20, y, lw, 60);
    create_monitor_row(hwnd, "MS:", IDC_MON_MS,
                       col2_x, y - 18, col2_lw, 80);

    y += create_monitor_row(hwnd, "PTYN:", IDC_MON_PTYN,
                            20, y, lw, 250);
    create_monitor_row(hwnd, "AF:", IDC_MON_AF,
                       col2_x, y - 18, col2_lw, 260);

    /* Full-width rows */
    y += create_monitor_row(hwnd, "RT:", IDC_MON_RT,
                            20, y, lw, 580);
    y += create_monitor_row(hwnd, "LPS:", IDC_MON_LPS,
                            20, y, lw, 580);
    y += create_monitor_row(hwnd, "eRT:", IDC_MON_ERT,
                            20, y, lw, 580);
    y += create_monitor_row(hwnd, "RT+1:", IDC_MON_RTP1,
                            20, y, lw, 580);
    y += create_monitor_row(hwnd, "RT+2:", IDC_MON_RTP2,
                            20, y, lw, 580);

    /* =================================================================
     * Log Group
     * ================================================================= */
    y = 753;
    create_groupbox(hwnd, "Log", lx, y, gw, WINDOW_HEIGHT - y - 10);

    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_READONLY,
        20, y + 18, gw - 20, WINDOW_HEIGHT - y - 35,
        hwnd, (HMENU)(intptr_t)IDC_LOG_EDIT, hinst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)g_mono_font, TRUE);

    /* Apply default font, then re-apply special fonts */
    EnumChildWindows(hwnd, (WNDENUMPROC)SetFont_EnumProc, (LPARAM)g_font);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)g_mono_font, TRUE);

    /* Re-apply mono font to all monitor value labels */
    int mon_ids[] = {
        IDC_MON_PI, IDC_MON_PS, IDC_MON_RT, IDC_MON_PTY, IDC_MON_PTYN,
        IDC_MON_TP, IDC_MON_TA, IDC_MON_MS, IDC_MON_LPS, IDC_MON_ERT,
        IDC_MON_RTP1, IDC_MON_RTP2, IDC_MON_AF
    };
    for (int i = 0; i < (int)(sizeof(mon_ids)/sizeof(mon_ids[0])); i++) {
        HWND h = GetDlgItem(hwnd, mon_ids[i]);
        if (h) SendMessageA(h, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }
}

static BOOL CALLBACK SetFont_EnumProc(HWND hwnd, LPARAM lParam) {
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Window Procedure
 * ----------------------------------------------------------------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        create_all_controls(hwnd);
        SetTimer(hwnd, IDT_LOG_TIMER, LOG_TIMER_MS, NULL);
        SetTimer(hwnd, IDT_FILEWATCH_TIMER, FILEWATCH_TIMER_MS, NULL);
        SetTimer(hwnd, IDT_MONITOR_TIMER, MONITOR_TIMER_MS, NULL);
        log_msg("MiniRDS GUI started. Version " VERSION "\r\n");
        log_msg("Select an audio device and click Start.\r\n");
        log_msg("File watch: set paths in RT/PS/RT+ fields, tick Watch.\r\n");
        log_msg("  RT file:  plain text, first line = RadioText\r\n");
        log_msg("  PS file:  plain text, auto-chunked into 8-char PS segments\r\n");
        log_msg("  RT+ file: \"Artist Name || Song Title\"\r\n");
        log_msg("  PT file:  PTY number or name, auto-updates Program Type\r\n");
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_START_BTN:   start_engine(); break;
        case IDC_STOP_BTN:    stop_engine(); break;
        case IDC_APPLY_BTN:   apply_settings(); break;
        case IDC_BROWSE_BTN:  browse_for_file(IDC_FILE_EDIT); break;
        case IDC_EXEC_BTN:    execute_command_file(); break;
        case IDC_RT_FILE_BROWSE:  browse_for_file(IDC_RT_FILE_EDIT); break;
        case IDC_PS_FILE_BROWSE:  browse_for_file(IDC_PS_FILE_EDIT); break;
        case IDC_RTP_FILE_BROWSE: browse_for_file(IDC_RTP_FILE_EDIT); break;
        case IDC_PT_FILE_BROWSE:  browse_for_file(IDC_PT_FILE_EDIT); break;
        }
        return 0;

    case WM_HSCROLL: {
        HWND slider = GetDlgItem(hwnd, IDC_VOLUME_SLIDER);
        if ((HWND)lParam == slider) {
            int pos = (int)SendMessageA(slider, TBM_GETPOS, 0, 0);
            char label[16];
            snprintf(label, sizeof(label), "%d%%", pos);
            SetDlgItemTextA(hwnd, IDC_VOLUME_LABEL, label);
            if (g_engine_running)
                set_output_volume((float)pos);
        }
        return 0;
    }

    case WM_TIMER:
        switch (wParam) {
        case IDT_LOG_TIMER:       drain_stderr_to_log(); break;
        case IDT_FILEWATCH_TIMER: check_file_watches(); break;
        case IDT_MONITOR_TIMER:   update_monitor(); break;
        }
        return 0;

    case WM_APP + 1:
        /* Engine stopped */
        SetDlgItemTextA(hwnd, IDC_STATUS_LABEL, "Status: Stopped");
        EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_DEVICE_COMBO), TRUE);
        /* Clear monitor */
        {
            int ids[] = {
                IDC_MON_PI, IDC_MON_PS, IDC_MON_RT, IDC_MON_PTY, IDC_MON_PTYN,
                IDC_MON_TP, IDC_MON_TA, IDC_MON_MS, IDC_MON_LPS, IDC_MON_ERT,
                IDC_MON_RTP1, IDC_MON_RTP2, IDC_MON_AF
            };
            for (int i = 0; i < (int)(sizeof(ids)/sizeof(ids[0])); i++)
                SetDlgItemTextA(hwnd, ids[i], "");
        }
        return 0;

    case WM_CLOSE:
        if (g_engine_running) stop_engine();
        KillTimer(hwnd, IDT_LOG_TIMER);
        KillTimer(hwnd, IDT_FILEWATCH_TIMER);
        KillTimer(hwnd, IDT_MONITOR_TIMER);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* -----------------------------------------------------------------------
 * Entry Point
 * ----------------------------------------------------------------------- */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXA wc;
    MSG msg;
    RECT rc;

    (void)hPrevInstance;
    (void)lpCmdLine;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    g_bold_font = CreateFontA(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    g_mono_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    setup_stderr_capture();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MiniRDSGUI";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    rc.left = 0;
    rc.top = 0;
    rc.right = WINDOW_WIDTH;
    rc.bottom = WINDOW_HEIGHT;
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);

    g_hwnd = CreateWindowExA(0, "MiniRDSGUI", APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);

    if (!g_hwnd) {
        MessageBoxA(NULL, "Failed to create main window.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (g_font) DeleteObject(g_font);
    if (g_bold_font) DeleteObject(g_bold_font);
    if (g_mono_font) DeleteObject(g_mono_font);
    if (g_stderr_read) CloseHandle(g_stderr_read);

    return (int)msg.wParam;
}

#else /* !_WIN32 */

#include <stdio.h>

int main(void) {
    fprintf(stderr, "The MiniRDS GUI is only available on Windows.\n");
    fprintf(stderr, "Use the command-line 'minirds' instead.\n");
    return 1;
}

#endif /* _WIN32 */
