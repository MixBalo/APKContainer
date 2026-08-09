/*
 * log_file.c — per-run log writer
 *
 * Status: REAL. Writes timestamped, tagged, leveled lines to a file under
 *         <Application Support>/APKLive/logs/, mirrors to os_log, rotates
 *         old runs (keeps 5). Thread-safe via a pthread mutex.
 *
 * Part of APKLive Phase 2.
 */
#include "log_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <os/log.h>

#define LOG_MAX_LINE   4096
#define LOG_MAX_PATH   1024
#define LOG_KEEP_COUNT 5

static pthread_mutex_t s_lock  = PTHREAD_MUTEX_INITIALIZER;
static FILE           *s_fp    = NULL;
static char            s_path[LOG_MAX_PATH] = {0};
static char            s_dir[LOG_MAX_PATH]  = {0};
static int             s_inited = 0;
static os_log_t        s_oslog  = NULL;

static const char *level_str(int lvl) {
    switch (lvl) {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO ";
        case LOG_LVL_WARN:  return "WARN ";
        case LOG_LVL_ERROR: return "ERROR";
        default:            return "?????";
    }
}

static os_log_type_t level_oslog(int lvl) {
    switch (lvl) {
        case LOG_LVL_DEBUG: return OS_LOG_TYPE_DEBUG;
        case LOG_LVL_INFO:  return OS_LOG_TYPE_INFO;
        case LOG_LVL_WARN:  return OS_LOG_TYPE_DEFAULT;
        case LOG_LVL_ERROR: return OS_LOG_TYPE_ERROR;
        default:            return OS_LOG_TYPE_DEFAULT;
    }
}

static void default_dir(char *out, size_t cap) {
    /* <CWD>/../../Library/Application Support/APKLive/logs — we can't easily
     * get NSApplicationSupportDirectory from C, so use a sane default that
     * matches what Swift CatalogStore sets up. Swift will call log_set_dir()
     * at startup with the real path; this is just a fallback. */
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, cap, "%s/Library/Application Support/APKLive/logs", home);
}

static void ensure_dir(const char *path) {
    /* mkdir -p */
    char tmp[LOG_MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int name_ts_compare(const struct dirent **a, const struct dirent **b) {
    /* Reverse-chronological: newest first. Names look like apklive-<epochms>.log */
    return -strcmp((*a)->d_name, (*b)->d_name);
}

void log_rotate(int keep_count) {
    char dir[LOG_MAX_PATH];
    pthread_mutex_lock(&s_lock);
    strncpy(dir, s_dir[0] ? s_dir : "", sizeof(dir) - 1);
    pthread_mutex_unlock(&s_lock);
    if (!dir[0]) return;

    struct dirent **names = NULL;
    int n = scandir(dir, &names, NULL, name_ts_compare);
    if (n < 0) return;
    int deleted = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i]->d_name;
        if (nm[0] == '.') continue;
        if (strncmp(nm, "apklive-", 8) != 0) continue;
        if (i < keep_count) { free(names[i]); continue; }
        char full[LOG_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, nm);
        if (unlink(full) == 0) deleted++;
        free(names[i]);
    }
    free(names);
    if (deleted > 0) {
        log_write(LOG_LVL_INFO, "log", "rotated out %d old log file(s)", deleted);
    }
}

void log_set_dir(const char *abs_path) {
    pthread_mutex_lock(&s_lock);
    strncpy(s_dir, abs_path ? abs_path : "", sizeof(s_dir) - 1);
    s_dir[sizeof(s_dir) - 1] = '\0';
    pthread_mutex_unlock(&s_lock);
}

void log_init(void) {
    pthread_mutex_lock(&s_lock);
    if (s_inited) { pthread_mutex_unlock(&s_lock); return; }
    s_inited = 1;

    if (!s_oslog) s_oslog = os_log_create("com.you.apklive", "runtime");

    if (!s_dir[0]) default_dir(s_dir, sizeof(s_dir));
    ensure_dir(s_dir);

    /* filename: apklive-<epochms>.log */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ms = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    snprintf(s_path, sizeof(s_path), "%s/apklive-%lld.log", s_dir, ms);

    s_fp = fopen(s_path, "a");
    if (!s_fp) {
        /* Fallback to stderr so we never lose logs. */
        fprintf(stderr, "log_init: fopen(%s) failed: %s — falling back to stderr\n",
                s_path, strerror(errno));
        s_fp = stderr;
        strncpy(s_path, "(stderr)", sizeof(s_path) - 1);
    }
    fprintf(s_fp, "==== APKLive log start (pid=%d) ====\n", (int)getpid());
    fflush(s_fp);
    pthread_mutex_unlock(&s_lock);

    log_rotate(LOG_KEEP_COUNT);
    LOGI("log", "log file: %s", s_path);
}

const char *log_current_path(void) {
    return s_path[0] ? s_path : "(not initialized)";
}

void log_write(int level, const char *tag, const char *fmt, ...) {
    if (!tag) tag = "?";

    char buf[LOG_MAX_LINE];
    int off = 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ [%s] [%s] ",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec,
                    (int)(ts.tv_nsec / 1000000),
                    level_str(level), tag);

    va_list ap;
    va_start(ap, fmt);
    off += vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);

    if (off >= (int)sizeof(buf) - 2) off = (int)sizeof(buf) - 2;
    buf[off++] = '\n';
    buf[off] = '\0';

    pthread_mutex_lock(&s_lock);
    if (!s_inited) {
        s_inited = 1;
        if (!s_oslog) s_oslog = os_log_create("com.you.apklive", "runtime");
        if (!s_dir[0]) default_dir(s_dir, sizeof(s_dir));
        ensure_dir(s_dir);
        /* Lazy init: open a fallback file if log_init() was never called. */
        if (!s_fp) {
            struct timespec t2; clock_gettime(CLOCK_REALTIME, &t2);
            long long ms = (long long)t2.tv_sec * 1000LL + t2.tv_nsec / 1000000LL;
            snprintf(s_path, sizeof(s_path), "%s/apklive-%lld.log", s_dir, ms);
            s_fp = fopen(s_path, "a");
            if (!s_fp) { s_fp = stderr; strncpy(s_path, "(stderr)", sizeof(s_path)-1); }
        }
    }
    if (s_fp) {
        fputs(buf, s_fp);
        fflush(s_fp);   /* flush every line — we want logs even on a crash */
    }
    pthread_mutex_unlock(&s_lock);

    /* Mirror to os_log so Console.app and `log stream` see it. */
    if (s_oslog) {
        os_log_with_type(s_oslog, level_oslog(level), "%{public}s", buf);
    }
}

void log_shutdown(void) {
    pthread_mutex_lock(&s_lock);
    if (s_fp && s_fp != stderr) {
        fprintf(s_fp, "==== APKLive log end ====\n");
        fclose(s_fp);
    }
    s_fp = NULL;
    s_inited = 0;
    pthread_mutex_unlock(&s_lock);
}
