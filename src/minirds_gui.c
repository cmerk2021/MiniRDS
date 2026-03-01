/*
 * MiniRDS GUI - Production Windows GUI for MiniRDS
 *
 * Three-window architecture:
 *   1. Main Window     - Compact display with quick controls
 *   2. Settings Window - Organized parameter editing with sections
 *   3. Diagnostics     - Full parameter dump, level meter, log
 *
 * Features:
 *   - Auto-restart on encoder failure with exponential backoff
 *   - INI-based settings save/load
 *   - Real-time RT+ artist/title extraction display
 *   - PTY selector by RBDS program type name
 *   - File watch with manual/file source toggles
 *   - Peak level metering
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
#include <stdarg.h>
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

/* =======================================================================
 * Constants
 * ======================================================================= */

#define APP_TITLE           "MiniRDS"
#define MAIN_WINDOW_W       520
#define MAIN_WINDOW_H       400
#define SETTINGS_WINDOW_W   700
#define SETTINGS_WINDOW_H   870
#define DIAG_WINDOW_W       720
#define DIAG_WINDOW_H       700

#define MAX_AUTO_RESTARTS       5
#define RESTART_COOLDOWN_BASE   500
#define RESTART_COOLDOWN_MAX    10000

#define INI_FILE_NAME       "minirds.ini"
#define INI_SECTION         "MiniRDS"

/* Timer IDs */
#define IDT_LOG_TIMER       1
#define IDT_MONITOR_TIMER   2
#define IDT_FILEWATCH_TIMER 3
#define IDT_AUTOSAVE_TIMER  4
#define LOG_TIMER_MS        100
#define MONITOR_TIMER_MS    200
#define FILEWATCH_TIMER_MS  500
#define AUTOSAVE_DELAY_MS   1500

/* =======================================================================
 * Control IDs - Main Window
 * ======================================================================= */
enum {
    IDC_M_FIRST = 100,
    IDC_M_STATUS_LABEL,
    IDC_M_PS_VAL, IDC_M_RT_VAL, IDC_M_PTY_VAL,
    IDC_M_ARTIST_LABEL, IDC_M_ARTIST_VAL,
    IDC_M_TITLE_LABEL, IDC_M_TITLE_VAL,
    IDC_M_START_BTN, IDC_M_STOP_BTN,
    IDC_M_TA_BTN, IDC_M_MS_BTN, IDC_M_TP_BTN,
    IDC_M_VOL_SLIDER, IDC_M_VOL_LABEL,
    IDC_M_SETTINGS_BTN, IDC_M_DIAG_BTN,
};

/* =======================================================================
 * Control IDs - Settings Window
 * ======================================================================= */
enum {
    IDC_S_FIRST = 300,
    /* Station Identity */
    IDC_S_PI_EDIT, IDC_S_PS_EDIT, IDC_S_ECC_EDIT,
    /* Radio Text */
    IDC_S_RT_MANUAL_RADIO, IDC_S_RT_FILE_RADIO,
    IDC_S_RT_EDIT, IDC_S_RT_FILE_EDIT, IDC_S_RT_FILE_BROWSE,
    /* RT+ */
    IDC_S_RTP_MANUAL_RADIO, IDC_S_RTP_FILE_RADIO,
    IDC_S_RTP_FILE_EDIT, IDC_S_RTP_FILE_BROWSE,
    IDC_S_RTP_TAG1_TYPE, IDC_S_RTP_TAG1_START, IDC_S_RTP_TAG1_LEN,
    IDC_S_RTP_TAG2_TYPE, IDC_S_RTP_TAG2_START, IDC_S_RTP_TAG2_LEN,
    IDC_S_RTP_RUNNING_CHK, IDC_S_RTP_TOGGLE_CHK,
    /* Program Type */
    IDC_S_PTY_COMBO, IDC_S_PTYN_EDIT,
    /* Audio */
    IDC_S_DEVICE_COMBO, IDC_S_VOL_SLIDER, IDC_S_VOL_LABEL,
    /* Flags */
    IDC_S_TP_CHK, IDC_S_TA_CHK, IDC_S_MS_CHK,
    /* AF */
    IDC_S_AF_EDIT,
    /* Extended */
    IDC_S_LPS_EDIT, IDC_S_ERT_EDIT,
    /* File Watch: PS */
    IDC_S_PS_FILE_CHK, IDC_S_PS_FILE_EDIT, IDC_S_PS_FILE_BROWSE,
    /* File Watch: PT */
    IDC_S_PT_FILE_CHK, IDC_S_PT_FILE_EDIT, IDC_S_PT_FILE_BROWSE,
    /* Commands */
    IDC_S_CMD_EDIT, IDC_S_CMD_BROWSE, IDC_S_CMD_EXEC,
    /* PS segment duration */
    IDC_S_PS_SEGMENT_EDIT,
    /* Buttons */
    IDC_S_APPLY_BTN, IDC_S_SAVE_BTN, IDC_S_LOAD_BTN,
};

/* =======================================================================
 * Control IDs - Diagnostics Window
 * ======================================================================= */
enum {
    IDC_D_FIRST = 600,
    IDC_D_PI, IDC_D_PS, IDC_D_RT, IDC_D_PTY, IDC_D_PTYN,
    IDC_D_TP, IDC_D_TA, IDC_D_MS, IDC_D_AF,
    IDC_D_LPS, IDC_D_ERT,
    IDC_D_RTP1, IDC_D_RTP2, IDC_D_RTP_STATUS,
    IDC_D_UPTIME, IDC_D_ITERATIONS, IDC_D_RESTARTS,
    IDC_D_PEAK_BAR,
    IDC_D_PEAK_LABEL,
    IDC_D_LOG_EDIT,
};

/* =======================================================================
 * Global State
 * ======================================================================= */

/* Window handles */
static HWND g_main_hwnd;
static HWND g_settings_hwnd;
static HWND g_diag_hwnd;
static HWND g_log_edit;

/* Fonts */
static HFONT g_font;
static HFONT g_font_bold;
static HFONT g_font_mono;
static HFONT g_font_large;
static HFONT g_font_title;

/* Engine state */
static volatile LONG g_engine_running;
static volatile LONG g_stop_engine;
static HANDLE g_engine_thread;
static volatile LONG g_total_restarts;
static DWORD g_engine_start_tick;
static volatile unsigned long g_loop_count;

/* Peak level (0-1000 scaled) */
static volatile LONG g_peak_level;

/* Audio devices */
#define MAX_AUDIO_DEVICES 32
static struct {
    UINT id;
    char name[MAXPNAMELEN];
} g_audio_devices[MAX_AUDIO_DEVICES];
static int g_num_audio_devices;
static int g_selected_device = -1;
static float g_volume = 50.0f;

/* stderr capture */
static HANDLE g_stderr_read;
static HANDLE g_stderr_write;

/* File watch state */
static struct {
    char path[MAX_PATH];
    FILETIME last_write;
    BOOL active;
} g_rt_file, g_ps_file, g_rtp_file, g_pt_file;

/* RT source mode: 0=manual, 1=file */
static int g_rt_source;
/* RTP source mode: 0=manual, 1=file */
static int g_rtp_source;

/* PS chunking */
static struct {
    char full_text[512];
    char chunks[64][PS_LENGTH + 1];
    int num_chunks;
    int current_chunk;
    DWORD last_advance_tick;
} g_ps_scroll;
static DWORD g_ps_segment_ms = 4000; /* customizable segment duration */

/* Auto-save dirty flag */
static BOOL g_autosave_dirty = FALSE;
static BOOL g_loading_settings = FALSE; /* suppress auto-save during load */

/* INI file path */
static char g_ini_path[MAX_PATH];

/* RBDS PTY names for the combo box */
static const char *g_pty_names[] = {
#ifdef RBDS
    "0 - None", "1 - News", "2 - Information", "3 - Sports",
    "4 - Talk", "5 - Rock", "6 - Classic Rock", "7 - Adult Hits",
    "8 - Soft Rock", "9 - Top 40", "10 - Country", "11 - Oldies",
    "12 - Soft Music", "13 - Nostalgia", "14 - Jazz", "15 - Classical",
    "16 - R&B", "17 - Soft R&B", "18 - Language", "19 - Religious Music",
    "20 - Religious Talk", "21 - Personality", "22 - Public", "23 - College",
    "24 - Spanish Talk", "25 - Spanish Music", "26 - Hip-Hop", "27 - Unassigned",
    "28 - Unassigned", "29 - Weather", "30 - Emergency Test", "31 - Emergency"
#else
    "0 - None", "1 - News", "2 - Current Affairs", "3 - Information",
    "4 - Sport", "5 - Education", "6 - Drama", "7 - Culture",
    "8 - Science", "9 - Varied", "10 - Pop Music", "11 - Rock Music",
    "12 - Easy Listening", "13 - Light Classical", "14 - Serious Classical",
    "15 - Other Music", "16 - Weather", "17 - Finance",
    "18 - Children's Programs", "19 - Social Affairs", "20 - Religion",
    "21 - Phone-in", "22 - Travel", "23 - Leisure", "24 - Jazz Music",
    "25 - Country Music", "26 - National Music", "27 - Oldies Music",
    "28 - Folk Music", "29 - Documentary", "30 - Alarm Test", "31 - Alarm"
#endif
};

/* RT+ content type names for combo boxes */
static const char *g_rtp_type_names[] = {
    "0 - DUMMY", "1 - ITEM.TITLE", "2 - ITEM.ALBUM",
    "3 - ITEM.TRACKNUMBER", "4 - ITEM.ARTIST", "5 - ITEM.COMPOSITION",
    "6 - ITEM.MOVEMENT", "7 - ITEM.CONDUCTOR", "8 - ITEM.COMPOSER",
    "9 - ITEM.BAND", "10 - ITEM.COMMENT", "11 - ITEM.GENRE",
    "12 - INFO.NEWS", "13 - INFO.NEWS.LOCAL", "14 - INFO.STOCKMARKET",
    "15 - INFO.SPORT", "16 - INFO.LOTTERY", "17 - INFO.HOROSCOPE",
    "18 - INFO.DAILY_DIVERSION", "19 - INFO.HEALTH", "20 - INFO.EVENT",
    "21 - INFO.SCENE", "22 - INFO.CINEMA", "23 - INFO.TV",
    "24 - INFO.DATE_TIME", "25 - INFO.WEATHER", "26 - INFO.TRAFFIC",
    "27 - INFO.ALARM", "28 - INFO.ADVERTISEMENT", "29 - INFO.URL",
    "30 - INFO.OTHER", "31 - STATIONNAME.SHORT"
};
#define NUM_RTP_TYPES 32

/* =======================================================================
 * Forward Declarations
 * ======================================================================= */
static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK DiagWndProc(HWND, UINT, WPARAM, LPARAM);
static void show_settings_window(HINSTANCE hInst);
static void show_diag_window(HINSTANCE hInst);
static void start_engine(void);
static void stop_engine(void);
static void apply_settings_from_gui(void);

/* =======================================================================
 * SECTION: Logging & Stderr Capture
 * ======================================================================= */

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
        } else break;
    }
}

/* =======================================================================
 * SECTION: Audio Device Enumeration
 * ======================================================================= */

static void enumerate_audio_devices(HWND combo) {
    UINT num_devs = waveOutGetNumDevs();
    WAVEOUTCAPSA caps;
    g_num_audio_devices = 0;
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"(System Default)");
    for (UINT i = 0; i < num_devs && g_num_audio_devices < MAX_AUDIO_DEVICES; i++) {
        if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            g_audio_devices[g_num_audio_devices].id = i;
            snprintf(g_audio_devices[g_num_audio_devices].name, MAXPNAMELEN, "%s", caps.szPname);
            char label[128];
            snprintf(label, sizeof(label), "%u: %s", i, caps.szPname);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)label);
            g_num_audio_devices++;
        }
    }
    SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

/* =======================================================================
 * SECTION: Sample Conversion
 * ======================================================================= */

static void float2char2channel(float *inbuf, char *outbuf, size_t frames) {
    size_t j = 0, k = 0;
    int16_t sample;
    int8_t lower, upper;
    for (size_t i = 0; i < frames; i++) {
        sample = (int16_t)lroundf((inbuf[j] + inbuf[j+1]) * 16383.5f);
        lower = sample & 0xFF;
        upper = (sample >> 8) & 0xFF;
        outbuf[k+0] = lower;
        outbuf[k+1] = upper;
        outbuf[k+2] = lower;
        outbuf[k+3] = upper;
        j += 2;
        k += 4;
    }
}

/* =======================================================================
 * SECTION: PS Smart-Split Text Chunking
 *
 * Packs as many full words as possible into each 8-character PS segment.
 * If a word doesn't fit in the remaining space, start a new segment.
 * If a single word is longer than 8 characters, truncate it with a
 * trailing dash '-' and continue the remainder on the next segment(s).
 * Each segment is space-padded to exactly PS_LENGTH (8) characters.
 * ======================================================================= */

static void ps_chunk_text(const char *text) {
    int ci = 0;
    char copy[512];
    char *words[256];
    int nwords = 0;

    size_t len = strlen(text);
    if (len == 0) { g_ps_scroll.num_chunks = 0; return; }

    /* Work on a mutable copy */
    strncpy(copy, text, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    len = strlen(copy);

    /* Trim trailing whitespace */
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

    if (nwords == 0) { g_ps_scroll.num_chunks = 0; return; }

    /* If the entire text fits in PS_LENGTH, just use it as-is */
    if (len <= PS_LENGTH) {
        memset(g_ps_scroll.chunks[0], ' ', PS_LENGTH);
        memcpy(g_ps_scroll.chunks[0], text, len > PS_LENGTH ? PS_LENGTH : len);
        g_ps_scroll.chunks[0][PS_LENGTH] = '\0';
        g_ps_scroll.num_chunks = 1;
        g_ps_scroll.current_chunk = 0;
        g_ps_scroll.last_advance_tick = GetTickCount();
        return;
    }

    /* Smart-split: pack as many full words as possible into each segment */
    char seg[PS_LENGTH + 1];
    int seg_used = 0; /* characters used in current segment */

    memset(seg, ' ', PS_LENGTH);
    seg[PS_LENGTH] = '\0';

    for (int w = 0; w < nwords && ci < 64; w++) {
        size_t wlen = strlen(words[w]);
        size_t wpos = 0; /* position within the current word (for long-word continuation) */

        while (wpos < wlen && ci < 64) {
            size_t rem_word = wlen - wpos; /* remaining chars in this word */
            int space_left = PS_LENGTH - seg_used;

            /* Need a separator space if segment already has content */
            int need_sep = (seg_used > 0) ? 1 : 0;
            int avail = space_left - need_sep;

            if (avail <= 0 && seg_used > 0) {
                /* Current segment is full, flush it */
                memcpy(g_ps_scroll.chunks[ci], seg, PS_LENGTH);
                g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
                ci++;
                memset(seg, ' ', PS_LENGTH);
                seg_used = 0;
                continue; /* re-evaluate this word in the new segment */
            }

            if (wpos == 0 && rem_word <= (size_t)avail) {
                /* Entire word fits in the remaining space */
                if (need_sep) {
                    seg[seg_used] = ' ';
                    seg_used++;
                }
                memcpy(seg + seg_used, words[w], rem_word);
                seg_used += (int)rem_word;
                break; /* done with this word */
            }

            if (wpos > 0 && rem_word <= (size_t)(PS_LENGTH - seg_used)) {
                /* Continuation of a long word — remainder fits */
                memcpy(seg + seg_used, words[w] + wpos, rem_word);
                seg_used += (int)rem_word;
                break; /* done with this word */
            }

            if (wpos == 0 && rem_word > (size_t)avail && seg_used > 0) {
                /* Word doesn't fit — flush current segment first if
                   the word could start a fresh segment (avoids splitting
                   a word that would fit on a blank line) */
                if (rem_word <= PS_LENGTH) {
                    memcpy(g_ps_scroll.chunks[ci], seg, PS_LENGTH);
                    g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
                    ci++;
                    memset(seg, ' ', PS_LENGTH);
                    seg_used = 0;
                    continue; /* retry in fresh segment */
                }
            }

            /* Word is too long for remaining space — truncate with dash */
            if (seg_used > 0 && need_sep) {
                /* If we already have content, flush first to give max room */
                memcpy(g_ps_scroll.chunks[ci], seg, PS_LENGTH);
                g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
                ci++;
                memset(seg, ' ', PS_LENGTH);
                seg_used = 0;
                continue;
            }

            /* Fill this segment with as much of the word as possible + dash */
            {
                int fill = PS_LENGTH - seg_used;
                if (rem_word > (size_t)fill) {
                    /* Need a dash — copy fill-1 chars + dash */
                    int copy_n = fill - 1;
                    if (copy_n > 0)
                        memcpy(seg + seg_used, words[w] + wpos, copy_n);
                    seg[seg_used + copy_n] = '-';
                    seg_used = PS_LENGTH;
                    wpos += copy_n;
                } else {
                    memcpy(seg + seg_used, words[w] + wpos, rem_word);
                    seg_used += (int)rem_word;
                    break;
                }
            }

            /* Flush the segment with the truncated word */
            memcpy(g_ps_scroll.chunks[ci], seg, PS_LENGTH);
            g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
            ci++;
            memset(seg, ' ', PS_LENGTH);
            seg_used = 0;
        }
    }

    /* Flush last segment if it has content */
    if (seg_used > 0 && ci < 64) {
        memcpy(g_ps_scroll.chunks[ci], seg, PS_LENGTH);
        g_ps_scroll.chunks[ci][PS_LENGTH] = '\0';
        ci++;
    }

    g_ps_scroll.num_chunks = ci;
    g_ps_scroll.current_chunk = 0;
    g_ps_scroll.last_advance_tick = GetTickCount();
}

static void ps_scroll_tick(void) {
    if (g_ps_scroll.num_chunks <= 1) return;
    if (!g_engine_running) return;
    DWORD now = GetTickCount();
    if ((now - g_ps_scroll.last_advance_tick) >= g_ps_segment_ms) {
        g_ps_scroll.current_chunk = (g_ps_scroll.current_chunk + 1) % g_ps_scroll.num_chunks;
        set_rds_ps(xlat((unsigned char *)g_ps_scroll.chunks[g_ps_scroll.current_chunk]));
        g_ps_scroll.last_advance_tick = now;
    }
}

/* =======================================================================
 * SECTION: File Watch Helpers
 * ======================================================================= */

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
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    while (rd > 0 && (buf[rd-1] == '\n' || buf[rd-1] == '\r' ||
           buf[rd-1] == ' ' || buf[rd-1] == '\t'))
        buf[--rd] = '\0';
    return buf;
}

static void process_rt_file(void) {
    char *text = read_file_text(g_rt_file.path);
    if (!text) return;
    if (text[0]) {
        set_rds_rt(xlat((unsigned char *)text));
        log_msg("[RT File] Updated: \"%s\"\r\n", text);
    }
    free(text);
}

static void process_ps_file(void) {
    char *text = read_file_text(g_ps_file.path);
    if (!text) return;
    if (text[0]) {
        strncpy(g_ps_scroll.full_text, text, sizeof(g_ps_scroll.full_text) - 1);
        g_ps_scroll.full_text[sizeof(g_ps_scroll.full_text) - 1] = '\0';
        ps_chunk_text(text);
        if (g_ps_scroll.num_chunks > 0) {
            set_rds_ps(xlat((unsigned char *)g_ps_scroll.chunks[0]));
            log_msg("[PS File] Loaded %d chunk(s)\r\n", g_ps_scroll.num_chunks);
        }
    }
    free(text);
}

static void process_rtp_file(void) {
    char *text = read_file_text(g_rtp_file.path);
    if (!text) return;
    if (!text[0]) { free(text); return; }

    char *sep = strstr(text, "||");
    if (!sep) {
        log_msg("[RT+ File] No '||' separator found. Format: artist || title\r\n");
        free(text); return;
    }

    char artist[RT_LENGTH + 1], title_str[RT_LENGTH + 1];
    size_t alen = sep - text;
    while (alen > 0 && text[alen - 1] == ' ') alen--;
    if (alen >= RT_LENGTH) alen = RT_LENGTH - 1;
    memcpy(artist, text, alen);
    artist[alen] = '\0';

    char *ts = sep + 2;
    while (*ts == ' ') ts++;
    strncpy(title_str, ts, RT_LENGTH);
    title_str[RT_LENGTH] = '\0';

    /* Look up the positions in the current RT */
    struct rds_params_t p;
    get_rds_params_copy(&p);
    char current_rt[RT_LENGTH + 1];
    memcpy(current_rt, p.rt, RT_LENGTH);
    current_rt[RT_LENGTH] = '\0';
    {
        int i;
        for (i = RT_LENGTH - 1; i >= 0 && current_rt[i] == ' '; i--)
            current_rt[i] = '\0';
    }

    size_t rt_len = strlen(current_rt);
    size_t a_len = strlen(artist), t_len = strlen(title_str);
    char *artist_pos = NULL, *title_pos = NULL;

    if (a_len > 0 && a_len <= rt_len) {
        for (size_t i = 0; i <= rt_len - a_len; i++) {
            if (strncasecmp(current_rt + i, artist, a_len) == 0) {
                artist_pos = current_rt + i; break;
            }
        }
    }
    if (t_len > 0 && t_len <= rt_len) {
        for (size_t i = 0; i <= rt_len - t_len; i++) {
            if (strncasecmp(current_rt + i, title_str, t_len) == 0) {
                title_pos = current_rt + i; break;
            }
        }
    }

    if (!artist_pos && !title_pos) {
        log_msg("[RT+ File] Artist/title not found in RT\r\n");
        free(text); return;
    }

    uint8_t tags[6] = {
        artist_pos ? 4 : 0,
        artist_pos ? (uint8_t)(artist_pos - current_rt) : 0,
        artist_pos ? (uint8_t)(a_len > 0 ? a_len - 1 : 0) : 0,
        title_pos ? 1 : 0,
        title_pos ? (uint8_t)(title_pos - current_rt) : 0,
        title_pos ? (uint8_t)(t_len > 0 ? t_len - 1 : 0) : 0
    };
    set_rds_rtplus_tags(tags);
    set_rds_rtplus_flags(3); /* running + toggle */
    log_msg("[RT+ File] Artist: \"%s\", Title: \"%s\"\r\n", artist, title_str);
    free(text);
}

static void process_pt_file(void) {
    char *text = read_file_text(g_pt_file.path);
    if (!text) return;
    if (text[0]) {
        uint8_t pty = (text[0] >= 'A') ? get_pty_code(text) : (uint8_t)strtoul(text, NULL, 10);
        set_rds_pty(pty);
        log_msg("[PT File] PTY set to %u (%s)\r\n", pty, get_pty_str(pty));
    }
    free(text);
}

static void check_file_watches(void) {
    FILETIME ft;
    if (g_rt_file.active && g_rt_file.path[0]) {
        if (get_file_write_time(g_rt_file.path, &ft) &&
            file_time_changed(&ft, &g_rt_file.last_write)) {
            g_rt_file.last_write = ft;
            if (g_engine_running) process_rt_file();
        }
    }
    if (g_ps_file.active && g_ps_file.path[0]) {
        if (get_file_write_time(g_ps_file.path, &ft) &&
            file_time_changed(&ft, &g_ps_file.last_write)) {
            g_ps_file.last_write = ft;
            if (g_engine_running) process_ps_file();
        }
    }
    if (g_rtp_file.active && g_rtp_file.path[0]) {
        if (get_file_write_time(g_rtp_file.path, &ft) &&
            file_time_changed(&ft, &g_rtp_file.last_write)) {
            g_rtp_file.last_write = ft;
            if (g_engine_running) process_rtp_file();
        }
    }
    if (g_pt_file.active && g_pt_file.path[0]) {
        if (get_file_write_time(g_pt_file.path, &ft) &&
            file_time_changed(&ft, &g_pt_file.last_write)) {
            g_pt_file.last_write = ft;
            if (g_engine_running) process_pt_file();
        }
    }
    ps_scroll_tick();
}

/* =======================================================================
 * SECTION: INI Settings Save / Load
 * ======================================================================= */

static void save_settings_ini(void) {
    char buf[512];
    HWND hw = g_settings_hwnd;
    if (!hw) return;

    GetDlgItemTextA(hw, IDC_S_PI_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "PI", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_PS_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "PS", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_RT_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "RT", buf, g_ini_path);

    {
        int pty_sel = (int)SendDlgItemMessageA(hw, IDC_S_PTY_COMBO, CB_GETCURSEL, 0, 0);
        snprintf(buf, sizeof(buf), "%d", pty_sel >= 0 ? pty_sel : 0);
        WritePrivateProfileStringA(INI_SECTION, "PTY", buf, g_ini_path);
    }

    GetDlgItemTextA(hw, IDC_S_PTYN_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "PTYN", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d",
        IsDlgButtonChecked(hw, IDC_S_TP_CHK) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringA(INI_SECTION, "TP", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d",
        IsDlgButtonChecked(hw, IDC_S_TA_CHK) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringA(INI_SECTION, "TA", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d",
        IsDlgButtonChecked(hw, IDC_S_MS_CHK) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringA(INI_SECTION, "MS", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_AF_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "AF", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_LPS_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "LPS", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_ERT_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "ERT", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_ECC_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "ECC", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_rt_source);
    WritePrivateProfileStringA(INI_SECTION, "RTSource", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_RT_FILE_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "RTFile", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_rtp_source);
    WritePrivateProfileStringA(INI_SECTION, "RTPSource", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_RTP_FILE_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "RTPFile", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d",
        IsDlgButtonChecked(hw, IDC_S_PS_FILE_CHK) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringA(INI_SECTION, "PSFileWatch", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_PS_FILE_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "PSFile", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d",
        IsDlgButtonChecked(hw, IDC_S_PT_FILE_CHK) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringA(INI_SECTION, "PTFileWatch", buf, g_ini_path);

    GetDlgItemTextA(hw, IDC_S_PT_FILE_EDIT, buf, sizeof(buf));
    WritePrivateProfileStringA(INI_SECTION, "PTFile", buf, g_ini_path);

    {
        int vol = (int)SendDlgItemMessageA(hw, IDC_S_VOL_SLIDER, TBM_GETPOS, 0, 0);
        snprintf(buf, sizeof(buf), "%d", vol);
        WritePrivateProfileStringA(INI_SECTION, "Volume", buf, g_ini_path);
    }

    /* RT+ manual config */
    {
        int t;
        t = (int)SendDlgItemMessageA(hw, IDC_S_RTP_TAG1_TYPE, CB_GETCURSEL, 0, 0);
        snprintf(buf, sizeof(buf), "%d", t >= 0 ? t : 0);
        WritePrivateProfileStringA(INI_SECTION, "RTPTag1Type", buf, g_ini_path);
        GetDlgItemTextA(hw, IDC_S_RTP_TAG1_START, buf, sizeof(buf));
        WritePrivateProfileStringA(INI_SECTION, "RTPTag1Start", buf, g_ini_path);
        GetDlgItemTextA(hw, IDC_S_RTP_TAG1_LEN, buf, sizeof(buf));
        WritePrivateProfileStringA(INI_SECTION, "RTPTag1Len", buf, g_ini_path);
        t = (int)SendDlgItemMessageA(hw, IDC_S_RTP_TAG2_TYPE, CB_GETCURSEL, 0, 0);
        snprintf(buf, sizeof(buf), "%d", t >= 0 ? t : 0);
        WritePrivateProfileStringA(INI_SECTION, "RTPTag2Type", buf, g_ini_path);
        GetDlgItemTextA(hw, IDC_S_RTP_TAG2_START, buf, sizeof(buf));
        WritePrivateProfileStringA(INI_SECTION, "RTPTag2Start", buf, g_ini_path);
        GetDlgItemTextA(hw, IDC_S_RTP_TAG2_LEN, buf, sizeof(buf));
        WritePrivateProfileStringA(INI_SECTION, "RTPTag2Len", buf, g_ini_path);

        snprintf(buf, sizeof(buf), "%d",
            IsDlgButtonChecked(hw, IDC_S_RTP_RUNNING_CHK) == BST_CHECKED ? 1 : 0);
        WritePrivateProfileStringA(INI_SECTION, "RTPRunning", buf, g_ini_path);
        snprintf(buf, sizeof(buf), "%d",
            IsDlgButtonChecked(hw, IDC_S_RTP_TOGGLE_CHK) == BST_CHECKED ? 1 : 0);
        WritePrivateProfileStringA(INI_SECTION, "RTPToggle", buf, g_ini_path);
    }

    /* PS segment duration */
    snprintf(buf, sizeof(buf), "%u", (unsigned)g_ps_segment_ms);
    WritePrivateProfileStringA(INI_SECTION, "PSSegmentMs", buf, g_ini_path);

    log_msg("Settings saved to %s\r\n", g_ini_path);
}

static void load_settings_ini(void) {
    char buf[512];
    HWND hw = g_settings_hwnd;
    if (!hw) return;

    g_loading_settings = TRUE; /* suppress auto-save during load */

    GetPrivateProfileStringA(INI_SECTION, "PI", "1000", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_PI_EDIT, buf);

    GetPrivateProfileStringA(INI_SECTION, "PS", "MiniRDS", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_PS_EDIT, buf);

    GetPrivateProfileStringA(INI_SECTION, "RT", "MiniRDS: Software RDS encoder",
        buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_RT_EDIT, buf);

    {
        int pty = GetPrivateProfileIntA(INI_SECTION, "PTY", 0, g_ini_path);
        SendDlgItemMessageA(hw, IDC_S_PTY_COMBO, CB_SETCURSEL, pty, 0);
    }

    GetPrivateProfileStringA(INI_SECTION, "PTYN", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_PTYN_EDIT, buf);

    CheckDlgButton(hw, IDC_S_TP_CHK,
        GetPrivateProfileIntA(INI_SECTION, "TP", 0, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_S_TA_CHK,
        GetPrivateProfileIntA(INI_SECTION, "TA", 0, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_S_MS_CHK,
        GetPrivateProfileIntA(INI_SECTION, "MS", 1, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);

    GetPrivateProfileStringA(INI_SECTION, "AF", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_AF_EDIT, buf);

    GetPrivateProfileStringA(INI_SECTION, "LPS", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_LPS_EDIT, buf);

    GetPrivateProfileStringA(INI_SECTION, "ERT", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_ERT_EDIT, buf);

    GetPrivateProfileStringA(INI_SECTION, "ECC", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_ECC_EDIT, buf);

    g_rt_source = GetPrivateProfileIntA(INI_SECTION, "RTSource", 0, g_ini_path);
    CheckRadioButton(hw, IDC_S_RT_MANUAL_RADIO, IDC_S_RT_FILE_RADIO,
        g_rt_source ? IDC_S_RT_FILE_RADIO : IDC_S_RT_MANUAL_RADIO);
    EnableWindow(GetDlgItem(hw, IDC_S_RT_EDIT), !g_rt_source);
    EnableWindow(GetDlgItem(hw, IDC_S_RT_FILE_EDIT), g_rt_source);
    EnableWindow(GetDlgItem(hw, IDC_S_RT_FILE_BROWSE), g_rt_source);

    GetPrivateProfileStringA(INI_SECTION, "RTFile", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_RT_FILE_EDIT, buf);

    g_rtp_source = GetPrivateProfileIntA(INI_SECTION, "RTPSource", 0, g_ini_path);
    CheckRadioButton(hw, IDC_S_RTP_MANUAL_RADIO, IDC_S_RTP_FILE_RADIO,
        g_rtp_source ? IDC_S_RTP_FILE_RADIO : IDC_S_RTP_MANUAL_RADIO);

    GetPrivateProfileStringA(INI_SECTION, "RTPFile", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_RTP_FILE_EDIT, buf);

    CheckDlgButton(hw, IDC_S_PS_FILE_CHK,
        GetPrivateProfileIntA(INI_SECTION, "PSFileWatch", 0, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);
    GetPrivateProfileStringA(INI_SECTION, "PSFile", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_PS_FILE_EDIT, buf);

    CheckDlgButton(hw, IDC_S_PT_FILE_CHK,
        GetPrivateProfileIntA(INI_SECTION, "PTFileWatch", 0, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);
    GetPrivateProfileStringA(INI_SECTION, "PTFile", "", buf, sizeof(buf), g_ini_path);
    SetDlgItemTextA(hw, IDC_S_PT_FILE_EDIT, buf);

    {
        int vol = GetPrivateProfileIntA(INI_SECTION, "Volume", 50, g_ini_path);
        SendDlgItemMessageA(hw, IDC_S_VOL_SLIDER, TBM_SETPOS, TRUE, vol);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d%%", vol);
        SetDlgItemTextA(hw, IDC_S_VOL_LABEL, lbl);
    }

    /* RT+ manual config */
    {
        int t;
        t = GetPrivateProfileIntA(INI_SECTION, "RTPTag1Type", 0, g_ini_path);
        SendDlgItemMessageA(hw, IDC_S_RTP_TAG1_TYPE, CB_SETCURSEL, t, 0);
        GetPrivateProfileStringA(INI_SECTION, "RTPTag1Start", "0", buf, sizeof(buf), g_ini_path);
        SetDlgItemTextA(hw, IDC_S_RTP_TAG1_START, buf);
        GetPrivateProfileStringA(INI_SECTION, "RTPTag1Len", "0", buf, sizeof(buf), g_ini_path);
        SetDlgItemTextA(hw, IDC_S_RTP_TAG1_LEN, buf);
        t = GetPrivateProfileIntA(INI_SECTION, "RTPTag2Type", 0, g_ini_path);
        SendDlgItemMessageA(hw, IDC_S_RTP_TAG2_TYPE, CB_SETCURSEL, t, 0);
        GetPrivateProfileStringA(INI_SECTION, "RTPTag2Start", "0", buf, sizeof(buf), g_ini_path);
        SetDlgItemTextA(hw, IDC_S_RTP_TAG2_START, buf);
        GetPrivateProfileStringA(INI_SECTION, "RTPTag2Len", "0", buf, sizeof(buf), g_ini_path);
        SetDlgItemTextA(hw, IDC_S_RTP_TAG2_LEN, buf);

        CheckDlgButton(hw, IDC_S_RTP_RUNNING_CHK,
            GetPrivateProfileIntA(INI_SECTION, "RTPRunning", 0, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hw, IDC_S_RTP_TOGGLE_CHK,
            GetPrivateProfileIntA(INI_SECTION, "RTPToggle", 0, g_ini_path) ? BST_CHECKED : BST_UNCHECKED);
    }

    /* PS segment duration */
    {
        int seg_ms = GetPrivateProfileIntA(INI_SECTION, "PSSegmentMs", 4000, g_ini_path);
        if (seg_ms < 500) seg_ms = 500;
        if (seg_ms > 30000) seg_ms = 30000;
        g_ps_segment_ms = (DWORD)seg_ms;
        snprintf(buf, sizeof(buf), "%d", seg_ms);
        SetDlgItemTextA(hw, IDC_S_PS_SEGMENT_EDIT, buf);
    }

    log_msg("Settings loaded from %s\r\n", g_ini_path);

    g_loading_settings = FALSE;
}

/* Schedule an auto-save: resets a debounce timer so we don't write on every keystroke */
static void schedule_autosave(void) {
    if (g_loading_settings) return;
    g_autosave_dirty = TRUE;
    if (g_settings_hwnd)
        SetTimer(g_settings_hwnd, IDT_AUTOSAVE_TIMER, AUTOSAVE_DELAY_MS, NULL);
}

/* =======================================================================
 * SECTION: Engine Thread with Auto-Restart
 * ======================================================================= */

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
    int driver_id = -1;
    size_t frames;
    unsigned long loop_count = 0;
    int local_restart_count = 0;
    int restart_cooldown = RESTART_COOLDOWN_BASE;

    mpx_buffer = (float *)malloc(NUM_MPX_FRAMES_IN * 2 * sizeof(float));
    out_buffer = (float *)malloc(NUM_MPX_FRAMES_OUT * 2 * sizeof(float));
    dev_out = (char *)malloc(NUM_MPX_FRAMES_OUT * 2 * sizeof(int16_t));
    if (!mpx_buffer || !out_buffer || !dev_out) {
        fprintf(stderr, "Error: failed to allocate audio buffers.\n");
        goto engine_exit;
    }

    fm_mpx_init(MPX_SAMPLE_RATE);
    set_output_volume(g_volume);
    fprintf(stderr, "Baseband generator initialized at %d Hz.\n", MPX_SAMPLE_RATE);

    /* Init RDS encoder from settings window or defaults */
    {
        struct rds_params_t rds_params;
        char buf[256];
        HWND sw = g_settings_hwnd;

        memset(&rds_params, 0, sizeof(rds_params));
        memcpy(rds_params.ps, "MiniRDS ", PS_LENGTH);
        memcpy(rds_params.rt, "MiniRDS: Software RDS encoder", 29);
        rds_params.pi = 0x1000;

        if (sw) {
            GetDlgItemTextA(sw, IDC_S_PI_EDIT, buf, sizeof(buf));
            if (buf[0]) {
#ifdef RBDS
                if (buf[0] == 'K' || buf[0] == 'W' ||
                    buf[0] == 'k' || buf[0] == 'w')
                    rds_params.pi = callsign2pi((unsigned char *)buf);
                else
#endif
                    rds_params.pi = (uint16_t)strtoul(buf, NULL, 16);
            }
            GetDlgItemTextA(sw, IDC_S_PS_EDIT, buf, sizeof(buf));
            if (buf[0]) {
                unsigned char *x = xlat((unsigned char *)buf);
                memset(rds_params.ps, ' ', PS_LENGTH);
                memcpy(rds_params.ps, x, strlen((char *)x) < PS_LENGTH ? strlen((char *)x) : PS_LENGTH);
            }
            if (g_rt_source == 0) {
                GetDlgItemTextA(sw, IDC_S_RT_EDIT, buf, sizeof(buf));
                if (buf[0]) {
                    unsigned char *x = xlat((unsigned char *)buf);
                    memset(rds_params.rt, ' ', RT_LENGTH);
                    size_t rtlen = strlen((char *)x);
                    if (rtlen > RT_LENGTH) rtlen = RT_LENGTH;
                    memcpy(rds_params.rt, x, rtlen);
                }
            }
            {
                int pty_sel = (int)SendDlgItemMessageA(sw, IDC_S_PTY_COMBO, CB_GETCURSEL, 0, 0);
                if (pty_sel >= 0) rds_params.pty = (uint8_t)pty_sel;
            }
            rds_params.tp = (IsDlgButtonChecked(sw, IDC_S_TP_CHK) == BST_CHECKED) ? 1 : 0;

            GetDlgItemTextA(sw, IDC_S_PTYN_EDIT, buf, sizeof(buf));
            if (buf[0]) {
                unsigned char *x = xlat((unsigned char *)buf);
                memcpy(rds_params.ptyn, x, strlen((char *)x) < PTYN_LENGTH ? strlen((char *)x) : PTYN_LENGTH);
            }

            GetDlgItemTextA(sw, IDC_S_AF_EDIT, buf, sizeof(buf));
            if (buf[0]) {
                char *tok = strtok(buf, " ,;");
                while (tok) {
                    add_rds_af(&rds_params.af, strtof(tok, NULL));
                    tok = strtok(NULL, " ,;");
                }
            }
        }

        init_rds_encoder(rds_params);
        fprintf(stderr, "RDS encoder initialized (PI=%04X, PS=\"%.8s\").\n",
                rds_params.pi, rds_params.ps);
    }

    /* Apply extra settings */
    if (g_settings_hwnd) {
        char buf[256];
        if (IsDlgButtonChecked(g_settings_hwnd, IDC_S_TA_CHK) == BST_CHECKED)
            set_rds_ta(1);
        if (IsDlgButtonChecked(g_settings_hwnd, IDC_S_MS_CHK) == BST_CHECKED)
            set_rds_ms(1);
        else
            set_rds_ms(0);

        GetDlgItemTextA(g_settings_hwnd, IDC_S_LPS_EDIT, buf, sizeof(buf));
        if (buf[0]) set_rds_lps((unsigned char *)buf);
        GetDlgItemTextA(g_settings_hwnd, IDC_S_ERT_EDIT, buf, sizeof(buf));
        if (buf[0]) set_rds_ert((unsigned char *)buf);
        GetDlgItemTextA(g_settings_hwnd, IDC_S_ECC_EDIT, buf, sizeof(buf));
        if (buf[0]) set_rds_ecc((uint8_t)strtoul(buf, NULL, 16));

        /* RT+ manual config */
        if (g_rtp_source == 0) {
            int t1 = (int)SendDlgItemMessageA(g_settings_hwnd, IDC_S_RTP_TAG1_TYPE, CB_GETCURSEL, 0, 0);
            int t2 = (int)SendDlgItemMessageA(g_settings_hwnd, IDC_S_RTP_TAG2_TYPE, CB_GETCURSEL, 0, 0);
            if (t1 < 0) t1 = 0;
            if (t2 < 0) t2 = 0;
            char s1[8], l1[8], s2[8], l2[8];
            GetDlgItemTextA(g_settings_hwnd, IDC_S_RTP_TAG1_START, s1, sizeof(s1));
            GetDlgItemTextA(g_settings_hwnd, IDC_S_RTP_TAG1_LEN, l1, sizeof(l1));
            GetDlgItemTextA(g_settings_hwnd, IDC_S_RTP_TAG2_START, s2, sizeof(s2));
            GetDlgItemTextA(g_settings_hwnd, IDC_S_RTP_TAG2_LEN, l2, sizeof(l2));
            uint8_t tags[6] = {
                (uint8_t)t1, (uint8_t)strtoul(s1, NULL, 10), (uint8_t)strtoul(l1, NULL, 10),
                (uint8_t)t2, (uint8_t)strtoul(s2, NULL, 10), (uint8_t)strtoul(l2, NULL, 10)
            };
            set_rds_rtplus_tags(tags);
            uint8_t flags = 0;
            if (IsDlgButtonChecked(g_settings_hwnd, IDC_S_RTP_RUNNING_CHK) == BST_CHECKED) flags |= 2;
            if (IsDlgButtonChecked(g_settings_hwnd, IDC_S_RTP_TOGGLE_CHK) == BST_CHECKED) flags |= 1;
            set_rds_rtplus_flags(flags);
        }
    }

    /* Setup file watches */
    g_rt_file.active = (g_rt_source == 1);
    g_rtp_file.active = (g_rtp_source == 1);
    if (g_settings_hwnd) {
        GetDlgItemTextA(g_settings_hwnd, IDC_S_RT_FILE_EDIT, g_rt_file.path, MAX_PATH);
        GetDlgItemTextA(g_settings_hwnd, IDC_S_RTP_FILE_EDIT, g_rtp_file.path, MAX_PATH);
        g_ps_file.active = (IsDlgButtonChecked(g_settings_hwnd, IDC_S_PS_FILE_CHK) == BST_CHECKED);
        GetDlgItemTextA(g_settings_hwnd, IDC_S_PS_FILE_EDIT, g_ps_file.path, MAX_PATH);
        g_pt_file.active = (IsDlgButtonChecked(g_settings_hwnd, IDC_S_PT_FILE_CHK) == BST_CHECKED);
        GetDlgItemTextA(g_settings_hwnd, IDC_S_PT_FILE_EDIT, g_pt_file.path, MAX_PATH);
    }
    memset(&g_rt_file.last_write, 0, sizeof(FILETIME));
    memset(&g_ps_file.last_write, 0, sizeof(FILETIME));
    memset(&g_rtp_file.last_write, 0, sizeof(FILETIME));
    memset(&g_pt_file.last_write, 0, sizeof(FILETIME));

    /* Reset PS scroll state before initial file reads */
    g_ps_scroll.num_chunks = 0;
    g_ps_scroll.current_chunk = 0;

    /* Trigger initial file read if watches are active */
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
    driver_id = ao_default_driver_id();
    if (driver_id < 0) {
        fprintf(stderr, "Error: no usable audio driver found.\n");
        goto engine_cleanup;
    }

    if (g_selected_device >= 0) {
        char dev_id_str[16];
        snprintf(dev_id_str, sizeof(dev_id_str), "%d", g_selected_device);
        ao_append_option(&ao_opts, "id", dev_id_str);
    }

    device = ao_open_live(driver_id, &format, ao_opts);
    if (ao_opts) { ao_free_options(ao_opts); ao_opts = NULL; }
    if (!device) {
        fprintf(stderr, "Error: cannot open audio device.\n");
        goto engine_cleanup;
    }
    fprintf(stderr, "Audio device opened at %d Hz.\n", OUTPUT_SAMPLE_RATE);

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
    fprintf(stderr, "RDS output started successfully.\n");

    g_engine_start_tick = GetTickCount();

    /* ===== Main generation loop with auto-restart ===== */
    while (!g_stop_engine) {
        fm_rds_get_frames(mpx_buffer, NUM_MPX_FRAMES_IN);

        if (resample(src_state, src_data, &frames) < 0) {
            fprintf(stderr, "Error: resampler failed at iteration %lu.\n", loop_count);
            break; /* Resampler failure is fatal */
        }
        if (frames == 0) continue;

        /* Track peak level for diagnostics meter */
        {
            float peak = 0.0f;
            for (size_t i = 0; i < frames * 2; i++) {
                float v = fabsf(out_buffer[i]);
                if (v > peak) peak = v;
            }
            InterlockedExchange(&g_peak_level, (LONG)(peak * 1000.0f));
        }

        float2char2channel(out_buffer, dev_out, frames);

        if (!ao_play(device, dev_out, (uint_32)(frames * 2 * sizeof(int16_t)))) {
            fprintf(stderr, "Error: ao_play failed at iteration %lu.\n", loop_count);

            /* ===== AUTO-RESTART LOGIC ===== */
            if (local_restart_count >= MAX_AUTO_RESTARTS) {
                fprintf(stderr, "Max auto-restarts (%d) exceeded. Giving up.\n",
                    MAX_AUTO_RESTARTS);
                break;
            }

            local_restart_count++;
            InterlockedIncrement(&g_total_restarts);
            fprintf(stderr, "Auto-restart %d/%d in %d ms...\n",
                    local_restart_count, MAX_AUTO_RESTARTS, restart_cooldown);

            ao_close(device);
            device = NULL;
            Sleep(restart_cooldown);
            restart_cooldown = restart_cooldown * 2;
            if (restart_cooldown > RESTART_COOLDOWN_MAX)
                restart_cooldown = RESTART_COOLDOWN_MAX;

            if (g_stop_engine) break;

            ao_opts = NULL;
            if (g_selected_device >= 0) {
                char dev_id_str[16];
                snprintf(dev_id_str, sizeof(dev_id_str), "%d", g_selected_device);
                ao_append_option(&ao_opts, "id", dev_id_str);
            }
            device = ao_open_live(driver_id, &format, ao_opts);
            if (ao_opts) { ao_free_options(ao_opts); ao_opts = NULL; }

            if (!device) {
                fprintf(stderr, "Failed to reopen audio device.\n");
                continue; /* try again next iteration */
            }
            fprintf(stderr, "Audio device reopened successfully.\n");
            continue;
        }

        loop_count++;
        g_loop_count = loop_count;

        /* Reset restart counter after sustained success */
        if (loop_count % 1000 == 0 && local_restart_count > 0) {
            local_restart_count = 0;
            restart_cooldown = RESTART_COOLDOWN_BASE;
            fprintf(stderr, "Encoder stable, reset restart counter.\n");
        }
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

    InterlockedExchange(&g_peak_level, 0);
    InterlockedExchange(&g_engine_running, 0);
    PostMessageA(g_main_hwnd, WM_APP + 1, 0, 0);
    return 0;
}

/* =======================================================================
 * SECTION: Engine Control
 * ======================================================================= */

static void start_engine(void) {
    if (InterlockedCompareExchange(&g_engine_running, 1, 0) != 0) {
        log_msg("Engine is already running.\r\n");
        return;
    }
    InterlockedExchange((volatile LONG *)&g_stop_engine, 0);
    g_loop_count = 0;
    InterlockedExchange(&g_total_restarts, 0);

    /* Get device selection from settings */
    if (g_settings_hwnd) {
        HWND combo = GetDlgItem(g_settings_hwnd, IDC_S_DEVICE_COMBO);
        int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
        g_selected_device = (sel <= 0) ? -1 : (int)g_audio_devices[sel - 1].id;
        g_volume = (float)SendDlgItemMessageA(g_settings_hwnd, IDC_S_VOL_SLIDER, TBM_GETPOS, 0, 0);
    }

    g_engine_thread = CreateThread(NULL, 0, engine_thread_proc, NULL, 0, NULL);
    if (!g_engine_thread) {
        log_msg("Error: could not create engine thread.\r\n");
        InterlockedExchange(&g_engine_running, 0);
        return;
    }

    SetDlgItemTextA(g_main_hwnd, IDC_M_STATUS_LABEL, "  RUNNING");
    EnableWindow(GetDlgItem(g_main_hwnd, IDC_M_START_BTN), FALSE);
    EnableWindow(GetDlgItem(g_main_hwnd, IDC_M_STOP_BTN), TRUE);
    InvalidateRect(g_main_hwnd, NULL, TRUE);
    log_msg("Starting RDS engine...\r\n");
}

static void stop_engine(void) {
    if (!g_engine_running) return;
    log_msg("Stopping RDS engine...\r\n");
    InterlockedExchange((volatile LONG *)&g_stop_engine, 1);
    if (g_engine_thread) {
        WaitForSingleObject(g_engine_thread, 5000);
        CloseHandle(g_engine_thread);
        g_engine_thread = NULL;
    }
    SetDlgItemTextA(g_main_hwnd, IDC_M_STATUS_LABEL, "  STOPPED");
    EnableWindow(GetDlgItem(g_main_hwnd, IDC_M_START_BTN), TRUE);
    EnableWindow(GetDlgItem(g_main_hwnd, IDC_M_STOP_BTN), FALSE);
    InvalidateRect(g_main_hwnd, NULL, TRUE);
}

/* =======================================================================
 * SECTION: Apply Settings Live
 * ======================================================================= */

static void apply_settings_from_gui(void) {
    HWND hw = g_settings_hwnd;
    if (!hw) return;
    char buf[256];

    GetDlgItemTextA(hw, IDC_S_PI_EDIT, buf, sizeof(buf));
    if (buf[0]) {
        uint16_t pi;
#ifdef RBDS
        if (buf[0] == 'K' || buf[0] == 'W' ||
            buf[0] == 'k' || buf[0] == 'w')
            pi = callsign2pi((unsigned char *)buf);
        else
#endif
            pi = (uint16_t)strtoul(buf, NULL, 16);
        if (g_engine_running) set_rds_pi(pi);
    }

    GetDlgItemTextA(hw, IDC_S_PS_EDIT, buf, sizeof(buf));
    if (buf[0] && g_engine_running) {
        /* If PS file watch is active, re-chunk the file text instead of
           overwriting with the manual PS field */
        if (IsDlgButtonChecked(hw, IDC_S_PS_FILE_CHK) == BST_CHECKED &&
            g_ps_scroll.full_text[0]) {
            ps_chunk_text(g_ps_scroll.full_text);
            if (g_ps_scroll.num_chunks > 0)
                set_rds_ps(xlat((unsigned char *)g_ps_scroll.chunks[0]));
        } else {
            set_rds_ps(xlat((unsigned char *)buf));
        }
    }

    if (g_rt_source == 0) {
        GetDlgItemTextA(hw, IDC_S_RT_EDIT, buf, sizeof(buf));
        if (buf[0] && g_engine_running)
            set_rds_rt(xlat((unsigned char *)buf));
    }

    {
        int pty_sel = (int)SendDlgItemMessageA(hw, IDC_S_PTY_COMBO, CB_GETCURSEL, 0, 0);
        if (pty_sel >= 0 && g_engine_running)
            set_rds_pty((uint8_t)pty_sel);
    }

    GetDlgItemTextA(hw, IDC_S_PTYN_EDIT, buf, sizeof(buf));
    if (g_engine_running) {
        if (buf[0] && buf[0] != '-')
            set_rds_ptyn(xlat((unsigned char *)buf));
        else if (buf[0] == '-') {
            unsigned char e = 0;
            set_rds_ptyn(&e);
        }
    }

    if (g_engine_running) {
        set_rds_tp((IsDlgButtonChecked(hw, IDC_S_TP_CHK) == BST_CHECKED) ? 1 : 0);
        set_rds_ta((IsDlgButtonChecked(hw, IDC_S_TA_CHK) == BST_CHECKED) ? 1 : 0);
        set_rds_ms((IsDlgButtonChecked(hw, IDC_S_MS_CHK) == BST_CHECKED) ? 1 : 0);
    }

    GetDlgItemTextA(hw, IDC_S_LPS_EDIT, buf, sizeof(buf));
    if (buf[0] && g_engine_running) {
        if (buf[0] == '-') buf[0] = 0;
        set_rds_lps((unsigned char *)buf);
    }

    GetDlgItemTextA(hw, IDC_S_ERT_EDIT, buf, sizeof(buf));
    if (buf[0] && g_engine_running) {
        if (buf[0] == '-') buf[0] = 0;
        set_rds_ert((unsigned char *)buf);
    }

    GetDlgItemTextA(hw, IDC_S_ECC_EDIT, buf, sizeof(buf));
    if (buf[0] && g_engine_running)
        set_rds_ecc((uint8_t)strtoul(buf, NULL, 16));

    /* Volume */
    {
        float vol = (float)SendDlgItemMessageA(hw, IDC_S_VOL_SLIDER, TBM_GETPOS, 0, 0);
        if (g_engine_running) set_output_volume(vol);
        SendDlgItemMessageA(g_main_hwnd, IDC_M_VOL_SLIDER, TBM_SETPOS, TRUE, (int)vol);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d%%", (int)vol);
        SetDlgItemTextA(g_main_hwnd, IDC_M_VOL_LABEL, lbl);
    }

    /* RT+ manual config */
    if (g_rtp_source == 0 && g_engine_running) {
        int t1 = (int)SendDlgItemMessageA(hw, IDC_S_RTP_TAG1_TYPE, CB_GETCURSEL, 0, 0);
        int t2 = (int)SendDlgItemMessageA(hw, IDC_S_RTP_TAG2_TYPE, CB_GETCURSEL, 0, 0);
        if (t1 < 0) t1 = 0;
        if (t2 < 0) t2 = 0;
        char s1[8], l1[8], s2[8], l2[8];
        GetDlgItemTextA(hw, IDC_S_RTP_TAG1_START, s1, sizeof(s1));
        GetDlgItemTextA(hw, IDC_S_RTP_TAG1_LEN, l1, sizeof(l1));
        GetDlgItemTextA(hw, IDC_S_RTP_TAG2_START, s2, sizeof(s2));
        GetDlgItemTextA(hw, IDC_S_RTP_TAG2_LEN, l2, sizeof(l2));
        uint8_t tags[6] = {
            (uint8_t)t1, (uint8_t)strtoul(s1, NULL, 10), (uint8_t)strtoul(l1, NULL, 10),
            (uint8_t)t2, (uint8_t)strtoul(s2, NULL, 10), (uint8_t)strtoul(l2, NULL, 10)
        };
        set_rds_rtplus_tags(tags);
        uint8_t flags = 0;
        if (IsDlgButtonChecked(hw, IDC_S_RTP_RUNNING_CHK) == BST_CHECKED) flags |= 2;
        if (IsDlgButtonChecked(hw, IDC_S_RTP_TOGGLE_CHK) == BST_CHECKED) flags |= 1;
        set_rds_rtplus_flags(flags);
    }

    /* Update file watches */
    g_rt_file.active = (g_rt_source == 1);
    GetDlgItemTextA(hw, IDC_S_RT_FILE_EDIT, g_rt_file.path, MAX_PATH);
    g_rtp_file.active = (g_rtp_source == 1);
    GetDlgItemTextA(hw, IDC_S_RTP_FILE_EDIT, g_rtp_file.path, MAX_PATH);
    g_ps_file.active = (IsDlgButtonChecked(hw, IDC_S_PS_FILE_CHK) == BST_CHECKED);
    GetDlgItemTextA(hw, IDC_S_PS_FILE_EDIT, g_ps_file.path, MAX_PATH);
    g_pt_file.active = (IsDlgButtonChecked(hw, IDC_S_PT_FILE_CHK) == BST_CHECKED);
    GetDlgItemTextA(hw, IDC_S_PT_FILE_EDIT, g_pt_file.path, MAX_PATH);

    /* Sync main window toggles */
    CheckDlgButton(g_main_hwnd, IDC_M_TA_BTN,
        (IsDlgButtonChecked(hw, IDC_S_TA_CHK) == BST_CHECKED) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main_hwnd, IDC_M_MS_BTN,
        (IsDlgButtonChecked(hw, IDC_S_MS_CHK) == BST_CHECKED) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main_hwnd, IDC_M_TP_BTN,
        (IsDlgButtonChecked(hw, IDC_S_TP_CHK) == BST_CHECKED) ? BST_CHECKED : BST_UNCHECKED);

    /* Read PS segment duration */
    {
        char seg_buf[16];
        GetDlgItemTextA(hw, IDC_S_PS_SEGMENT_EDIT, seg_buf, sizeof(seg_buf));
        int ms = atoi(seg_buf);
        if (ms < 500) ms = 500;
        if (ms > 30000) ms = 30000;
        g_ps_segment_ms = (DWORD)ms;
    }

    log_msg("Settings applied.\r\n");
}

/* =======================================================================
 * SECTION: Monitor Updates
 * ======================================================================= */

static void update_main_monitor(void) {
    if (!g_engine_running) return;

    struct rds_params_t p;
    struct rds_rtplus_info_t rtp;
    char buf[256];

    get_rds_params_copy(&p);
    get_rds_rtplus_info(&rtp);

    /* PS */
    {
        char d[PS_LENGTH + 1];
        memcpy(d, p.ps, PS_LENGTH); d[PS_LENGTH] = '\0';
        SetDlgItemTextA(g_main_hwnd, IDC_M_PS_VAL, d);
    }

    /* RT */
    {
        char d[RT_LENGTH + 1];
        memcpy(d, p.rt, RT_LENGTH); d[RT_LENGTH] = '\0';
        for (int i = RT_LENGTH - 1; i >= 0 && (d[i] == ' ' || d[i] == '\r'); i--)
            d[i] = '\0';
        SetDlgItemTextA(g_main_hwnd, IDC_M_RT_VAL, d);
    }

    /* PTY with name */
    snprintf(buf, sizeof(buf), "%s (%u)", get_pty_str(p.pty), p.pty);
    SetDlgItemTextA(g_main_hwnd, IDC_M_PTY_VAL, buf);

    /* RT+ computed Artist and Title */
    {
        char rt_str[RT_LENGTH + 1];
        memcpy(rt_str, p.rt, RT_LENGTH);
        rt_str[RT_LENGTH] = '\0';
        for (int i = RT_LENGTH - 1; i >= 0 && (rt_str[i] == ' ' || rt_str[i] == '\r'); i--)
            rt_str[i] = '\0';

        char artist_buf[RT_LENGTH + 1];
        char title_buf[RT_LENGTH + 1];
        strcpy(artist_buf, "(none)");
        strcpy(title_buf, "(none)");

        if (rtp.running) {
            for (int t = 0; t < 2; t++) {
                uint8_t type = rtp.type[t];
                uint8_t start = rtp.start[t];
                uint8_t len = rtp.len[t] + 1; /* len field stores length-1 */
                if (type == 0) continue;
                if (start + len > (uint8_t)strlen(rt_str)) {
                    if (start < (uint8_t)strlen(rt_str))
                        len = (uint8_t)strlen(rt_str) - start;
                    else
                        continue;
                }

                char extracted[RT_LENGTH + 1];
                memcpy(extracted, rt_str + start, len);
                extracted[len] = '\0';

                if (type == 4) /* ITEM.ARTIST */
                    snprintf(artist_buf, sizeof(artist_buf), "%s", extracted);
                else if (type == 1) /* ITEM.TITLE */
                    snprintf(title_buf, sizeof(title_buf), "%s", extracted);
                else {
                    const char *tname = get_rtp_tag_name(type);
                    if (t == 0)
                        snprintf(artist_buf, sizeof(artist_buf), "%.16s: %.46s", tname ? tname : "?", extracted);
                    else
                        snprintf(title_buf, sizeof(title_buf), "%.16s: %.46s", tname ? tname : "?", extracted);
                }
            }
        }
        SetDlgItemTextA(g_main_hwnd, IDC_M_ARTIST_VAL, artist_buf);
        SetDlgItemTextA(g_main_hwnd, IDC_M_TITLE_VAL, title_buf);
    }

    /* Sync toggle states */
    CheckDlgButton(g_main_hwnd, IDC_M_TA_BTN, p.ta ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main_hwnd, IDC_M_MS_BTN, p.ms ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main_hwnd, IDC_M_TP_BTN, p.tp ? BST_CHECKED : BST_UNCHECKED);
}

static void update_diag_monitor(void) {
    if (!g_diag_hwnd || !g_engine_running) return;

    struct rds_params_t p;
    struct rds_rtplus_info_t rtp;
    char buf[512];

    get_rds_params_copy(&p);
    get_rds_rtplus_info(&rtp);

    snprintf(buf, sizeof(buf), "%04X", p.pi);
    SetDlgItemTextA(g_diag_hwnd, IDC_D_PI, buf);

    {
        char d[PS_LENGTH + 1];
        memcpy(d, p.ps, PS_LENGTH); d[PS_LENGTH] = '\0';
        SetDlgItemTextA(g_diag_hwnd, IDC_D_PS, d);
    }

    {
        char d[RT_LENGTH + 1];
        memcpy(d, p.rt, RT_LENGTH); d[RT_LENGTH] = '\0';
        for (int i = RT_LENGTH - 1; i >= 0 && (d[i] == ' ' || d[i] == '\r'); i--)
            d[i] = '\0';
        SetDlgItemTextA(g_diag_hwnd, IDC_D_RT, d);
    }

    snprintf(buf, sizeof(buf), "%u (%s)", p.pty, get_pty_str(p.pty));
    SetDlgItemTextA(g_diag_hwnd, IDC_D_PTY, buf);

    {
        char d[PTYN_LENGTH + 1];
        memcpy(d, p.ptyn, PTYN_LENGTH); d[PTYN_LENGTH] = '\0';
        for (int i = PTYN_LENGTH - 1; i >= 0 && d[i] == ' '; i--) d[i] = '\0';
        SetDlgItemTextA(g_diag_hwnd, IDC_D_PTYN, d[0] ? d : "(none)");
    }

    SetDlgItemTextA(g_diag_hwnd, IDC_D_TP, p.tp ? "ON" : "OFF");
    SetDlgItemTextA(g_diag_hwnd, IDC_D_TA, p.ta ? "ON" : "OFF");
    SetDlgItemTextA(g_diag_hwnd, IDC_D_MS, p.ms ? "Music" : "Speech");

    {
        char d[LPS_LENGTH + 1];
        memcpy(d, p.lps, LPS_LENGTH); d[LPS_LENGTH] = '\0';
        SetDlgItemTextA(g_diag_hwnd, IDC_D_LPS, d[0] ? d : "(none)");
    }

    {
        char d[ERT_LENGTH + 1];
        memcpy(d, p.ert, ERT_LENGTH); d[ERT_LENGTH] = '\0';
        for (int i = ERT_LENGTH - 1; i >= 0 && (d[i] == ' ' || d[i] == '\r'); i--)
            d[i] = '\0';
        SetDlgItemTextA(g_diag_hwnd, IDC_D_ERT, d[0] ? d : "(none)");
    }

    {
        char *af = show_af_list(p.af);
        SetDlgItemTextA(g_diag_hwnd, IDC_D_AF, (af && af[0]) ? af : "(none)");
    }

    /* RT+ diagnostic: show positions AND extracted text */
    {
        char rt_str[RT_LENGTH + 1];
        memcpy(rt_str, p.rt, RT_LENGTH); rt_str[RT_LENGTH] = '\0';
        for (int i = RT_LENGTH - 1; i >= 0 && (rt_str[i] == ' ' || rt_str[i] == '\r'); i--)
            rt_str[i] = '\0';

        if (rtp.running) {
            for (int t = 0; t < 2; t++) {
                char extracted[RT_LENGTH + 1] = "";
                uint8_t start = rtp.start[t];
                uint8_t len = rtp.len[t] + 1;
                size_t rt_len = strlen(rt_str);
                if (rtp.type[t] != 0 && start + len <= rt_len) {
                    memcpy(extracted, rt_str + start, len);
                    extracted[len] = '\0';
                }
                char *tname = get_rtp_tag_name(rtp.type[t]);
                snprintf(buf, sizeof(buf), "%s (start=%u, len=%u) \"%s\"",
                    tname ? tname : "DUMMY",
                    rtp.start[t], rtp.len[t], extracted);
                SetDlgItemTextA(g_diag_hwnd,
                    t == 0 ? IDC_D_RTP1 : IDC_D_RTP2, buf);
            }
            snprintf(buf, sizeof(buf), "Running: YES  Toggle: %u", rtp.toggle);
        } else {
            SetDlgItemTextA(g_diag_hwnd, IDC_D_RTP1, "(inactive)");
            SetDlgItemTextA(g_diag_hwnd, IDC_D_RTP2, "(inactive)");
            snprintf(buf, sizeof(buf), "Running: NO");
        }
        SetDlgItemTextA(g_diag_hwnd, IDC_D_RTP_STATUS, buf);
    }

    /* Engine stats */
    {
        DWORD elapsed = GetTickCount() - g_engine_start_tick;
        int secs = (int)(elapsed / 1000);
        int hrs = secs / 3600; secs %= 3600;
        int mins = secs / 60; secs %= 60;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins, secs);
        SetDlgItemTextA(g_diag_hwnd, IDC_D_UPTIME, buf);

        snprintf(buf, sizeof(buf), "%lu", g_loop_count);
        SetDlgItemTextA(g_diag_hwnd, IDC_D_ITERATIONS, buf);

        snprintf(buf, sizeof(buf), "%ld", (long)g_total_restarts);
        SetDlgItemTextA(g_diag_hwnd, IDC_D_RESTARTS, buf);
    }

    /* Peak meter */
    {
        LONG peak = g_peak_level;
        SendDlgItemMessageA(g_diag_hwnd, IDC_D_PEAK_BAR, PBM_SETPOS, peak, 0);
        float db = (peak > 0) ? 20.0f * log10f((float)peak / 1000.0f) : -60.0f;
        snprintf(buf, sizeof(buf), "%.1f dB", db);
        SetDlgItemTextA(g_diag_hwnd, IDC_D_PEAK_LABEL, buf);
    }
}

/* =======================================================================
 * SECTION: GUI Helpers
 * ======================================================================= */

static HWND mk_label(HWND p, const char *t, int x, int y, int w, int h) {
    return CreateWindowA("STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, p, NULL, GetModuleHandle(NULL), NULL);
}

static HWND mk_label_id(HWND p, int id, const char *t, int x, int y, int w, int h) {
    return CreateWindowA("STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static HWND mk_edit(HWND p, int id, const char *t, int x, int y, int w, int h, DWORD ex) {
    return CreateWindowA("EDIT", t,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ex,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static HWND mk_btn(HWND p, int id, const char *t, int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static HWND mk_chk(HWND p, int id, const char *t, int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static HWND mk_toggle(HWND p, int id, const char *t, int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static HWND mk_radio(HWND p, int id, const char *t, int x, int y, int w, int h, BOOL grp) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
    if (grp) style |= WS_GROUP;
    return CreateWindowA("BUTTON", t, style,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static HWND mk_group(HWND p, const char *t, int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", t,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, p, NULL, GetModuleHandle(NULL), NULL);
}

static HWND mk_combo(HWND p, int id, int x, int y, int w, int h) {
    return CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, h, p, (HMENU)(intptr_t)id, GetModuleHandle(NULL), NULL);
}

static void browse_for_file(HWND owner, int edit_id) {
    OPENFILENAMEA ofn;
    char fp[MAX_PATH] = "";
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fp;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn))
        SetDlgItemTextA(owner, edit_id, fp);
}

static BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam) {
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

/* =======================================================================
 * SECTION: Main Window
 * ======================================================================= */

static void create_main_controls(HWND hwnd) {
    HINSTANCE hi = GetModuleHandle(NULL);
    int w = MAIN_WINDOW_W - 20;
    HWND ctrl;
    int gy;

    /* Header area */
    mk_label(hwnd, "MiniRDS v" VERSION, 12, 8, 200, 20);
    mk_btn(hwnd, IDC_M_SETTINGS_BTN, "Settings", w - 145, 6, 72, 24);
    mk_btn(hwnd, IDC_M_DIAG_BTN, "Diagnostics", w - 68, 6, 82, 24);

    /* Status label with color */
    ctrl = mk_label_id(hwnd, IDC_M_STATUS_LABEL, "  STOPPED", 12, 36, 200, 22);
    SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font_title, TRUE);

    /* Now Playing display group */
    mk_group(hwnd, "Now Playing", 8, 62, w, 168);
    gy = 80;

    {
        HWND lbl;
        lbl = mk_label(hwnd, "PS:", 18, gy, 50, 18);
        SendMessageA(lbl, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        ctrl = mk_label_id(hwnd, IDC_M_PS_VAL, "", 70, gy, w - 78, 18);
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font_large, TRUE);
        gy += 24;

        lbl = mk_label(hwnd, "RT:", 18, gy, 50, 18);
        SendMessageA(lbl, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        ctrl = mk_label_id(hwnd, IDC_M_RT_VAL, "", 70, gy, w - 78, 18);
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        gy += 22;

        lbl = mk_label(hwnd, "PTY:", 18, gy, 50, 18);
        SendMessageA(lbl, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        ctrl = mk_label_id(hwnd, IDC_M_PTY_VAL, "", 70, gy, w - 78, 18);
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        gy += 28;

        /* RT+ artist/title */
        lbl = mk_label(hwnd, "Artist:", 18, gy, 50, 18);
        SendMessageA(lbl, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        ctrl = mk_label_id(hwnd, IDC_M_ARTIST_VAL, "(none)", 70, gy, w - 78, 18);
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        gy += 22;

        lbl = mk_label(hwnd, "Title:", 18, gy, 50, 18);
        SendMessageA(lbl, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        ctrl = mk_label_id(hwnd, IDC_M_TITLE_VAL, "(none)", 70, gy, w - 78, 18);
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    }

    /* Volume slider */
    {
        int vy = 238;
        mk_label(hwnd, "Volume:", 12, vy + 3, 50, 18);
        ctrl = CreateWindowA(TRACKBAR_CLASSA, "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
            65, vy, w - 110, 25, hwnd, (HMENU)(intptr_t)IDC_M_VOL_SLIDER, hi, NULL);
        SendMessageA(ctrl, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageA(ctrl, TBM_SETPOS, TRUE, 50);
        mk_label_id(hwnd, IDC_M_VOL_LABEL, "50%", w - 38, vy + 3, 40, 18);
    }

    /* Control buttons */
    {
        int by = 272;
        mk_btn(hwnd, IDC_M_START_BTN, "Start", 12, by, 80, 30);
        ctrl = mk_btn(hwnd, IDC_M_STOP_BTN, "Stop", 98, by, 80, 30);
        EnableWindow(ctrl, FALSE);

        mk_toggle(hwnd, IDC_M_TA_BTN, "TA", 210, by, 50, 30);
        mk_toggle(hwnd, IDC_M_MS_BTN, "M/S", 266, by, 55, 30);
        mk_toggle(hwnd, IDC_M_TP_BTN, "TP", 327, by, 50, 30);
    }

    /* Apply default font */
    EnumChildWindows(hwnd, (WNDENUMPROC)SetFontProc, (LPARAM)g_font);

    /* Re-apply special fonts */
    SendDlgItemMessageA(hwnd, IDC_M_STATUS_LABEL, WM_SETFONT, (WPARAM)g_font_title, TRUE);
    SendDlgItemMessageA(hwnd, IDC_M_PS_VAL, WM_SETFONT, (WPARAM)g_font_large, TRUE);
    SendDlgItemMessageA(hwnd, IDC_M_RT_VAL, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    SendDlgItemMessageA(hwnd, IDC_M_PTY_VAL, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    SendDlgItemMessageA(hwnd, IDC_M_ARTIST_VAL, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    SendDlgItemMessageA(hwnd, IDC_M_TITLE_VAL, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_main_hwnd = hwnd;
        create_main_controls(hwnd);
        SetTimer(hwnd, IDT_MONITOR_TIMER, MONITOR_TIMER_MS, NULL);
        SetTimer(hwnd, IDT_LOG_TIMER, LOG_TIMER_MS, NULL);
        SetTimer(hwnd, IDT_FILEWATCH_TIMER, FILEWATCH_TIMER_MS, NULL);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_M_START_BTN: start_engine(); break;
        case IDC_M_STOP_BTN: stop_engine(); break;
        case IDC_M_SETTINGS_BTN:
            show_settings_window(GetModuleHandle(NULL)); break;
        case IDC_M_DIAG_BTN:
            show_diag_window(GetModuleHandle(NULL)); break;
        case IDC_M_TA_BTN:
            if (g_engine_running) {
                BOOL chk = (IsDlgButtonChecked(hwnd, IDC_M_TA_BTN) == BST_CHECKED);
                set_rds_ta(chk ? 1 : 0);
                if (g_settings_hwnd)
                    CheckDlgButton(g_settings_hwnd, IDC_S_TA_CHK, chk ? BST_CHECKED : BST_UNCHECKED);
            }
            break;
        case IDC_M_MS_BTN:
            if (g_engine_running) {
                BOOL chk = (IsDlgButtonChecked(hwnd, IDC_M_MS_BTN) == BST_CHECKED);
                set_rds_ms(chk ? 1 : 0);
                if (g_settings_hwnd)
                    CheckDlgButton(g_settings_hwnd, IDC_S_MS_CHK, chk ? BST_CHECKED : BST_UNCHECKED);
            }
            break;
        case IDC_M_TP_BTN:
            if (g_engine_running) {
                BOOL chk = (IsDlgButtonChecked(hwnd, IDC_M_TP_BTN) == BST_CHECKED);
                set_rds_tp(chk ? 1 : 0);
                if (g_settings_hwnd)
                    CheckDlgButton(g_settings_hwnd, IDC_S_TP_CHK, chk ? BST_CHECKED : BST_UNCHECKED);
            }
            break;
        }
        return 0;

    case WM_HSCROLL: {
        HWND slider = GetDlgItem(hwnd, IDC_M_VOL_SLIDER);
        if ((HWND)lParam == slider) {
            int pos = (int)SendMessageA(slider, TBM_GETPOS, 0, 0);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%d%%", pos);
            SetDlgItemTextA(hwnd, IDC_M_VOL_LABEL, lbl);
            if (g_engine_running) set_output_volume((float)pos);
            if (g_settings_hwnd) {
                SendDlgItemMessageA(g_settings_hwnd, IDC_S_VOL_SLIDER, TBM_SETPOS, TRUE, pos);
                SetDlgItemTextA(g_settings_hwnd, IDC_S_VOL_LABEL, lbl);
            }
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctrl = (HWND)lParam;
        if (ctrl == GetDlgItem(hwnd, IDC_M_STATUS_LABEL)) {
            SetBkMode(hdc, TRANSPARENT);
            if (g_engine_running)
                SetTextColor(hdc, RGB(0, 160, 0));
            else
                SetTextColor(hdc, RGB(200, 0, 0));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }

    case WM_TIMER:
        switch (wParam) {
        case IDT_MONITOR_TIMER:
            update_main_monitor();
            update_diag_monitor();
            break;
        case IDT_LOG_TIMER:
            drain_stderr_to_log();
            break;
        case IDT_FILEWATCH_TIMER:
            check_file_watches();
            break;
        }
        return 0;

    case WM_APP + 1:
        /* Engine stopped (posted from engine thread) */
        SetDlgItemTextA(hwnd, IDC_M_STATUS_LABEL, "  STOPPED");
        EnableWindow(GetDlgItem(hwnd, IDC_M_START_BTN), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_M_STOP_BTN), FALSE);
        InvalidateRect(hwnd, NULL, TRUE);
        SetDlgItemTextA(hwnd, IDC_M_PS_VAL, "");
        SetDlgItemTextA(hwnd, IDC_M_RT_VAL, "");
        SetDlgItemTextA(hwnd, IDC_M_PTY_VAL, "");
        SetDlgItemTextA(hwnd, IDC_M_ARTIST_VAL, "(none)");
        SetDlgItemTextA(hwnd, IDC_M_TITLE_VAL, "(none)");
        return 0;

    case WM_CLOSE:
        if (g_engine_running) stop_engine();
        KillTimer(hwnd, IDT_MONITOR_TIMER);
        KillTimer(hwnd, IDT_LOG_TIMER);
        KillTimer(hwnd, IDT_FILEWATCH_TIMER);
        if (g_settings_hwnd) { DestroyWindow(g_settings_hwnd); g_settings_hwnd = NULL; }
        if (g_diag_hwnd) { DestroyWindow(g_diag_hwnd); g_diag_hwnd = NULL; }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* =======================================================================
 * SECTION: Settings Window
 * ======================================================================= */

static void create_settings_controls(HWND hwnd) {
    HINSTANCE hi = GetModuleHandle(NULL);
    int gw = SETTINGS_WINDOW_W - 30;
    int y = 5;
    HWND ctrl;

    /* Station Identity */
    mk_group(hwnd, "Station Identity", 10, y, gw, 62);
    y += 20;
    mk_label(hwnd, "PI:", 20, y + 3, 20, 18);
    mk_edit(hwnd, IDC_S_PI_EDIT, "1000", 42, y, 80, 22, 0);
    mk_label(hwnd, "PS:", 135, y + 3, 20, 18);
    mk_edit(hwnd, IDC_S_PS_EDIT, "MiniRDS", 158, y, 120, 22, 0);
    mk_label(hwnd, "ECC:", 295, y + 3, 30, 18);
    mk_edit(hwnd, IDC_S_ECC_EDIT, "", 328, y, 50, 22, 0);
    y += 50;

    /* Radio Text */
    mk_group(hwnd, "Radio Text", 10, y, gw, 90);
    y += 18;
    mk_radio(hwnd, IDC_S_RT_MANUAL_RADIO, "Manual", 20, y, 70, 20, TRUE);
    mk_radio(hwnd, IDC_S_RT_FILE_RADIO, "File Watch", 95, y, 90, 20, FALSE);
    CheckRadioButton(hwnd, IDC_S_RT_MANUAL_RADIO, IDC_S_RT_FILE_RADIO, IDC_S_RT_MANUAL_RADIO);
    y += 22;
    mk_label(hwnd, "Text:", 20, y + 3, 35, 18);
    mk_edit(hwnd, IDC_S_RT_EDIT, "MiniRDS: Software RDS encoder", 58, y, gw - 68, 22, 0);
    y += 26;
    mk_label(hwnd, "File:", 20, y + 3, 35, 18);
    ctrl = mk_edit(hwnd, IDC_S_RT_FILE_EDIT, "", 58, y, gw - 110, 22, 0);
    EnableWindow(ctrl, FALSE);
    ctrl = mk_btn(hwnd, IDC_S_RT_FILE_BROWSE, "...", gw - 42, y, 30, 22);
    EnableWindow(ctrl, FALSE);
    y += 30;

    /* RT+ Tags */
    mk_group(hwnd, "RT+ Tags", 10, y, gw, 140);
    y += 18;
    mk_radio(hwnd, IDC_S_RTP_MANUAL_RADIO, "Manual", 20, y, 70, 20, TRUE);
    mk_radio(hwnd, IDC_S_RTP_FILE_RADIO, "File Watch (artist || title)", 95, y, 210, 20, FALSE);
    CheckRadioButton(hwnd, IDC_S_RTP_MANUAL_RADIO, IDC_S_RTP_FILE_RADIO, IDC_S_RTP_MANUAL_RADIO);
    y += 22;
    mk_label(hwnd, "File:", 20, y + 3, 35, 18);
    mk_edit(hwnd, IDC_S_RTP_FILE_EDIT, "", 58, y, gw - 110, 22, 0);
    mk_btn(hwnd, IDC_S_RTP_FILE_BROWSE, "...", gw - 42, y, 30, 22);
    y += 26;
    mk_label(hwnd, "Tag 1:", 20, y + 3, 38, 18);
    ctrl = mk_combo(hwnd, IDC_S_RTP_TAG1_TYPE, 62, y, 200, 300);
    { int i; for (i = 0; i < NUM_RTP_TYPES; i++) SendMessageA(ctrl, CB_ADDSTRING, 0, (LPARAM)g_rtp_type_names[i]); }
    SendMessageA(ctrl, CB_SETCURSEL, 0, 0);
    mk_label(hwnd, "Start:", 270, y + 3, 35, 18);
    mk_edit(hwnd, IDC_S_RTP_TAG1_START, "0", 308, y, 40, 22, ES_NUMBER);
    mk_label(hwnd, "Len:", 355, y + 3, 28, 18);
    mk_edit(hwnd, IDC_S_RTP_TAG1_LEN, "0", 386, y, 40, 22, ES_NUMBER);
    y += 26;
    mk_label(hwnd, "Tag 2:", 20, y + 3, 38, 18);
    ctrl = mk_combo(hwnd, IDC_S_RTP_TAG2_TYPE, 62, y, 200, 300);
    { int i; for (i = 0; i < NUM_RTP_TYPES; i++) SendMessageA(ctrl, CB_ADDSTRING, 0, (LPARAM)g_rtp_type_names[i]); }
    SendMessageA(ctrl, CB_SETCURSEL, 0, 0);
    mk_label(hwnd, "Start:", 270, y + 3, 35, 18);
    mk_edit(hwnd, IDC_S_RTP_TAG2_START, "0", 308, y, 40, 22, ES_NUMBER);
    mk_label(hwnd, "Len:", 355, y + 3, 28, 18);
    mk_edit(hwnd, IDC_S_RTP_TAG2_LEN, "0", 386, y, 40, 22, ES_NUMBER);
    mk_chk(hwnd, IDC_S_RTP_RUNNING_CHK, "Running", 440, y, 70, 20);
    mk_chk(hwnd, IDC_S_RTP_TOGGLE_CHK, "Toggle", 515, y, 65, 20);
    y += 30;

    /* Program Type */
    mk_group(hwnd, "Program Type", 10, y, gw, 62);
    y += 20;
    mk_label(hwnd, "PTY:", 20, y + 3, 28, 18);
    ctrl = mk_combo(hwnd, IDC_S_PTY_COMBO, 52, y, 220, 400);
    { int i; for (i = 0; i < 32; i++) SendMessageA(ctrl, CB_ADDSTRING, 0, (LPARAM)g_pty_names[i]); }
    SendMessageA(ctrl, CB_SETCURSEL, 0, 0);
    mk_label(hwnd, "PTYN:", 290, y + 3, 35, 18);
    mk_edit(hwnd, IDC_S_PTYN_EDIT, "", 330, y, 120, 22, 0);
    y += 50;

    /* Flags */
    mk_group(hwnd, "Flags", 10, y, gw, 42);
    y += 18;
    mk_chk(hwnd, IDC_S_TP_CHK, "TP (Traffic Programme)", 20, y, 175, 20);
    mk_chk(hwnd, IDC_S_TA_CHK, "TA (Traffic Announcement)", 200, y, 190, 20);
    mk_chk(hwnd, IDC_S_MS_CHK, "Music/Speech", 400, y, 110, 20);
    CheckDlgButton(hwnd, IDC_S_MS_CHK, BST_CHECKED);
    y += 32;

    /* Alternative Frequencies */
    mk_group(hwnd, "Alternative Frequencies", 10, y, gw, 48);
    y += 20;
    mk_label(hwnd, "AF:", 20, y + 3, 20, 18);
    mk_edit(hwnd, IDC_S_AF_EDIT, "", 45, y, gw - 55, 22, 0);
    y += 36;

    /* Audio Output */
    mk_group(hwnd, "Audio Output", 10, y, gw, 72);
    y += 18;
    mk_label(hwnd, "Device:", 20, y + 3, 45, 18);
    ctrl = mk_combo(hwnd, IDC_S_DEVICE_COMBO, 68, y, gw - 78, 200);
    enumerate_audio_devices(ctrl);
    y += 26;
    mk_label(hwnd, "Volume:", 20, y + 3, 45, 18);
    ctrl = CreateWindowA(TRACKBAR_CLASSA, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        68, y, gw - 130, 25, hwnd, (HMENU)(intptr_t)IDC_S_VOL_SLIDER, hi, NULL);
    SendMessageA(ctrl, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageA(ctrl, TBM_SETPOS, TRUE, 50);
    SendMessageA(ctrl, TBM_SETTICFREQ, 10, 0);
    mk_label_id(hwnd, IDC_S_VOL_LABEL, "50%", gw - 52, y + 3, 40, 18);
    y += 36;

    /* Extended Text */
    mk_group(hwnd, "Extended Text", 10, y, gw, 72);
    y += 18;
    mk_label(hwnd, "LPS:", 20, y + 3, 28, 18);
    mk_edit(hwnd, IDC_S_LPS_EDIT, "", 52, y, gw - 62, 22, 0);
    y += 26;
    mk_label(hwnd, "eRT:", 20, y + 3, 28, 18);
    mk_edit(hwnd, IDC_S_ERT_EDIT, "", 52, y, gw - 62, 22, 0);
    y += 36;

    /* File Watch: PS */
    {
        int hw2 = gw / 2 - 5;
        mk_group(hwnd, "File Watch: PS (smart split)", 10, y, hw2, 76);
        int fy = y + 18;
        mk_chk(hwnd, IDC_S_PS_FILE_CHK, "Enable", 20, fy, 60, 20);
        mk_edit(hwnd, IDC_S_PS_FILE_EDIT, "", 85, fy, hw2 - 108, 22, 0);
        mk_btn(hwnd, IDC_S_PS_FILE_BROWSE, "...", hw2 - 16, fy, 28, 22);
        fy += 26;
        mk_label(hwnd, "Segment (ms):", 20, fy + 3, 82, 18);
        mk_edit(hwnd, IDC_S_PS_SEGMENT_EDIT, "4000", 106, fy, 55, 22, 0);

        /* File Watch: PT */
        int rx = gw / 2 + 10;
        mk_group(hwnd, "File Watch: PT", rx, y, hw2, 76);
        mk_chk(hwnd, IDC_S_PT_FILE_CHK, "Enable", rx + 10, y + 18, 60, 20);
        mk_edit(hwnd, IDC_S_PT_FILE_EDIT, "", rx + 75, y + 18, hw2 - 118, 22, 0);
        mk_btn(hwnd, IDC_S_PT_FILE_BROWSE, "...", rx + hw2 - 36, y + 18, 28, 22);
    }
    y += 82;

    /* Command File */
    mk_group(hwnd, "Command File", 10, y, gw, 48);
    y += 18;
    mk_edit(hwnd, IDC_S_CMD_EDIT, "", 20, y, gw - 170, 22, 0);
    mk_btn(hwnd, IDC_S_CMD_BROWSE, "Browse", gw - 142, y, 60, 22);
    mk_btn(hwnd, IDC_S_CMD_EXEC, "Execute", gw - 76, y, 60, 22);
    y += 36;

    /* Action buttons */
    mk_btn(hwnd, IDC_S_APPLY_BTN, "Apply Settings", 10, y, 120, 28);
    mk_btn(hwnd, IDC_S_SAVE_BTN, "Save to INI", 140, y, 100, 28);
    mk_btn(hwnd, IDC_S_LOAD_BTN, "Load from INI", 250, y, 110, 28);

    /* Apply font */
    EnumChildWindows(hwnd, (WNDENUMPROC)SetFontProc, (LPARAM)g_font);
}

static void execute_command_file(HWND hwnd) {
    char fp[MAX_PATH];
    FILE *f;
    char line[CMD_BUFFER_SIZE];
    int count = 0;
    GetDlgItemTextA(hwnd, IDC_S_CMD_EDIT, fp, MAX_PATH);
    if (!fp[0]) { log_msg("No command file specified.\r\n"); return; }
    f = fopen(fp, "r");
    if (!f) { log_msg("Error: cannot open file: %s\r\n", fp); return; }
    log_msg("Executing commands from: %s\r\n", fp);
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        log_msg("  CMD: %s\r\n", line);
        process_ascii_cmd((unsigned char *)line);
        count++;
    }
    fclose(f);
    log_msg("Executed %d commands.\r\n", count);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_settings_hwnd = hwnd;
        create_settings_controls(hwnd);
        if (GetFileAttributesA(g_ini_path) != INVALID_FILE_ATTRIBUTES)
            load_settings_ini();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_S_APPLY_BTN: apply_settings_from_gui(); break;
        case IDC_S_SAVE_BTN: save_settings_ini(); break;
        case IDC_S_LOAD_BTN: load_settings_ini(); break;
        case IDC_S_RT_FILE_BROWSE: browse_for_file(hwnd, IDC_S_RT_FILE_EDIT); schedule_autosave(); break;
        case IDC_S_RTP_FILE_BROWSE: browse_for_file(hwnd, IDC_S_RTP_FILE_EDIT); schedule_autosave(); break;
        case IDC_S_PS_FILE_BROWSE: browse_for_file(hwnd, IDC_S_PS_FILE_EDIT); schedule_autosave(); break;
        case IDC_S_PT_FILE_BROWSE: browse_for_file(hwnd, IDC_S_PT_FILE_EDIT); schedule_autosave(); break;
        case IDC_S_CMD_BROWSE: browse_for_file(hwnd, IDC_S_CMD_EDIT); break;
        case IDC_S_CMD_EXEC: execute_command_file(hwnd); break;

        case IDC_S_RT_MANUAL_RADIO:
            g_rt_source = 0;
            EnableWindow(GetDlgItem(hwnd, IDC_S_RT_EDIT), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_S_RT_FILE_EDIT), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_S_RT_FILE_BROWSE), FALSE);
            schedule_autosave();
            break;
        case IDC_S_RT_FILE_RADIO:
            g_rt_source = 1;
            EnableWindow(GetDlgItem(hwnd, IDC_S_RT_EDIT), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_S_RT_FILE_EDIT), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_S_RT_FILE_BROWSE), TRUE);
            schedule_autosave();
            break;

        case IDC_S_RTP_MANUAL_RADIO:
            g_rtp_source = 0;
            schedule_autosave();
            break;
        case IDC_S_RTP_FILE_RADIO:
            g_rtp_source = 1;
            schedule_autosave();
            break;

        default:
            /* Auto-save on any edit change (EN_CHANGE), checkbox click (BN_CLICKED), combo change (CBN_SELCHANGE) */
            if (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == CBN_SELCHANGE)
                schedule_autosave();
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_AUTOSAVE_TIMER) {
            KillTimer(hwnd, IDT_AUTOSAVE_TIMER);
            if (g_autosave_dirty) {
                g_autosave_dirty = FALSE;
                save_settings_ini();
            }
            return 0;
        }
        break;

    case WM_HSCROLL: {
        HWND slider = GetDlgItem(hwnd, IDC_S_VOL_SLIDER);
        if ((HWND)lParam == slider) {
            int pos = (int)SendMessageA(slider, TBM_GETPOS, 0, 0);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%d%%", pos);
            SetDlgItemTextA(hwnd, IDC_S_VOL_LABEL, lbl);
            if (g_engine_running) set_output_volume((float)pos);
            SendDlgItemMessageA(g_main_hwnd, IDC_M_VOL_SLIDER, TBM_SETPOS, TRUE, pos);
            SetDlgItemTextA(g_main_hwnd, IDC_M_VOL_LABEL, lbl);
            schedule_autosave();
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        g_settings_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void show_settings_window(HINSTANCE hInst) {
    if (g_settings_hwnd) {
        ShowWindow(g_settings_hwnd, SW_SHOW);
        SetForegroundWindow(g_settings_hwnd);
        return;
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MiniRDSSettings";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExA(&wc);

    RECT rc = {0, 0, SETTINGS_WINDOW_W, SETTINGS_WINDOW_H};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);

    CreateWindowExA(0, "MiniRDSSettings", "MiniRDS - Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        g_main_hwnd, NULL, hInst, NULL);

    if (g_settings_hwnd) {
        ShowWindow(g_settings_hwnd, SW_SHOW);
        UpdateWindow(g_settings_hwnd);
    }
}

/* =======================================================================
 * SECTION: Diagnostics Window
 * ======================================================================= */

static int diag_row(HWND hwnd, const char *label, int id, int x, int y, int lw, int vw) {
    HWND lbl = mk_label(hwnd, label, x, y, lw, 16);
    SendMessageA(lbl, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
    HWND val = mk_label_id(hwnd, id, "", x + lw + 4, y, vw, 16);
    SendMessageA(val, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    return 18;
}

static void create_diag_controls(HWND hwnd) {
    HINSTANCE hi = GetModuleHandle(NULL);
    int gw = DIAG_WINDOW_W - 30;
    int y = 5;
    int lw = 42;
    int c2x = 340, c2lw = 32;

    /* RDS Parameters */
    mk_group(hwnd, "RDS Parameters", 10, y, gw, 230);
    y += 18;

    y += diag_row(hwnd, "PI:", IDC_D_PI, 20, y, lw, 80);
    diag_row(hwnd, "TP:", IDC_D_TP, c2x, y - 18, c2lw, 50);

    y += diag_row(hwnd, "PS:", IDC_D_PS, 20, y, lw, 250);
    diag_row(hwnd, "TA:", IDC_D_TA, c2x, y - 18, c2lw, 50);

    y += diag_row(hwnd, "PTY:", IDC_D_PTY, 20, y, lw, 250);
    diag_row(hwnd, "MS:", IDC_D_MS, c2x, y - 18, c2lw, 80);

    y += diag_row(hwnd, "PTYN:", IDC_D_PTYN, 20, y, lw, 250);
    y += diag_row(hwnd, "RT:", IDC_D_RT, 20, y, lw, gw - 62);
    y += diag_row(hwnd, "LPS:", IDC_D_LPS, 20, y, lw, gw - 62);
    y += diag_row(hwnd, "eRT:", IDC_D_ERT, 20, y, lw, gw - 62);
    y += diag_row(hwnd, "AF:", IDC_D_AF, 20, y, lw, gw - 62);
    y += 8;

    /* RT+ Tags */
    mk_group(hwnd, "RT+ Tags (diagnostic)", 10, y, gw, 78);
    y += 18;
    y += diag_row(hwnd, "Tag1:", IDC_D_RTP1, 20, y, 38, gw - 68);
    y += diag_row(hwnd, "Tag2:", IDC_D_RTP2, 20, y, 38, gw - 68);
    y += diag_row(hwnd, "Stat:", IDC_D_RTP_STATUS, 20, y, 38, gw - 68);
    y += 8;

    /* Engine Status */
    mk_group(hwnd, "Engine Status", 10, y, gw, 58);
    y += 18;
    diag_row(hwnd, "Uptime:", IDC_D_UPTIME, 20, y, 50, 100);
    diag_row(hwnd, "Iter:", IDC_D_ITERATIONS, 180, y, 32, 120);
    diag_row(hwnd, "Restarts:", IDC_D_RESTARTS, 340, y, 58, 60);
    y += 22;

    /* Peak meter */
    mk_label(hwnd, "Output:", 20, y + 2, 50, 16);
    CreateWindowA(PROGRESS_CLASSA, "",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        75, y, gw - 150, 18, hwnd,
        (HMENU)(intptr_t)IDC_D_PEAK_BAR, hi, NULL);
    SendDlgItemMessageA(hwnd, IDC_D_PEAK_BAR, PBM_SETRANGE, 0, MAKELONG(0, 1000));
    SendDlgItemMessageA(hwnd, IDC_D_PEAK_BAR, PBM_SETBARCOLOR, 0, (LPARAM)RGB(0, 180, 0));
    mk_label_id(hwnd, IDC_D_PEAK_LABEL, "-inf dB", gw - 68, y + 2, 60, 16);
    y += 26;

    /* Log */
    mk_group(hwnd, "Log", 10, y, gw, DIAG_WINDOW_H - y - 15);
    y += 18;

    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_READONLY,
        20, y, gw - 20, DIAG_WINDOW_H - y - 25,
        hwnd, (HMENU)(intptr_t)IDC_D_LOG_EDIT, hi, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

    /* Apply default font, then re-apply specials */
    EnumChildWindows(hwnd, (WNDENUMPROC)SetFontProc, (LPARAM)g_font);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

    {
        int ids[] = { IDC_D_PI, IDC_D_PS, IDC_D_RT, IDC_D_PTY, IDC_D_PTYN,
                      IDC_D_TP, IDC_D_TA, IDC_D_MS, IDC_D_LPS, IDC_D_ERT, IDC_D_AF,
                      IDC_D_RTP1, IDC_D_RTP2, IDC_D_RTP_STATUS,
                      IDC_D_UPTIME, IDC_D_ITERATIONS, IDC_D_RESTARTS, IDC_D_PEAK_LABEL };
        int i;
        for (i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
            HWND h = GetDlgItem(hwnd, ids[i]);
            if (h) SendMessageA(h, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        }
    }
}

static LRESULT CALLBACK DiagWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_diag_hwnd = hwnd;
        create_diag_controls(hwnd);
        log_msg("MiniRDS Diagnostics opened. Version " VERSION "\r\n");
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_log_edit = NULL;
        return 0;

    case WM_SHOWWINDOW:
        if (wParam)
            g_log_edit = GetDlgItem(hwnd, IDC_D_LOG_EDIT);
        break;

    case WM_DESTROY:
        g_log_edit = NULL;
        g_diag_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void show_diag_window(HINSTANCE hInst) {
    if (g_diag_hwnd) {
        ShowWindow(g_diag_hwnd, SW_SHOW);
        SetForegroundWindow(g_diag_hwnd);
        g_log_edit = GetDlgItem(g_diag_hwnd, IDC_D_LOG_EDIT);
        return;
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DiagWndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MiniRDSDiag";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExA(&wc);

    RECT rc = {0, 0, DIAG_WINDOW_W, DIAG_WINDOW_H};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);

    CreateWindowExA(0, "MiniRDSDiag", "MiniRDS - Diagnostics",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        g_main_hwnd, NULL, hInst, NULL);

    if (g_diag_hwnd) {
        ShowWindow(g_diag_hwnd, SW_SHOW);
        UpdateWindow(g_diag_hwnd);
    }
}

/* =======================================================================
 * SECTION: Entry Point
 * ======================================================================= */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXA wc;
    MSG msg;
    RECT rc;

    (void)hPrevInstance;
    (void)lpCmdLine;

    /* DPI awareness */
    {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            typedef BOOL (WINAPI *SPDA)(void);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            SPDA fn = (SPDA)GetProcAddress(user32, "SetProcessDPIAware");
#pragma GCC diagnostic pop
            if (fn) fn();
        }
    }

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icc);
    }

    /* Create fonts */
    g_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    g_font_bold = CreateFontA(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    g_font_mono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    g_font_large = CreateFontA(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    g_font_title = CreateFontA(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    /* INI file path (beside exe) */
    {
        GetModuleFileNameA(NULL, g_ini_path, MAX_PATH);
        char *last_slash = strrchr(g_ini_path, '\\');
        if (last_slash) last_slash[1] = '\0';
        strcat(g_ini_path, INI_FILE_NAME);
    }

    setup_stderr_capture();

    /* Register main window class */
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MiniRDSMain";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    rc.left = 0; rc.top = 0;
    rc.right = MAIN_WINDOW_W;
    rc.bottom = MAIN_WINDOW_H;
    AdjustWindowRectEx(&rc,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    g_main_hwnd = CreateWindowExA(0, "MiniRDSMain",
        APP_TITLE " v" VERSION,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);

    if (!g_main_hwnd) {
        MessageBoxA(NULL, "Failed to create main window.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_main_hwnd, nCmdShow);
    UpdateWindow(g_main_hwnd);

    /* Auto-open settings on first launch */
    show_settings_window(hInstance);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        BOOL handled = FALSE;
        if (g_settings_hwnd && IsDialogMessageA(g_settings_hwnd, &msg))
            handled = TRUE;
        if (!handled && g_diag_hwnd && IsDialogMessageA(g_diag_hwnd, &msg))
            handled = TRUE;
        if (!handled) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    /* Cleanup */
    if (g_font) DeleteObject(g_font);
    if (g_font_bold) DeleteObject(g_font_bold);
    if (g_font_mono) DeleteObject(g_font_mono);
    if (g_font_large) DeleteObject(g_font_large);
    if (g_font_title) DeleteObject(g_font_title);
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
