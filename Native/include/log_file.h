/*
 * log_file.h — per-run log writer shared by every native module
 *
 * Status: REAL. Writes timestamped, tagged, leveled lines to a rotating log
 *         file under the app's Application Support/APKLive/logs/ directory,
 *         AND mirrors to os_log. Swift reads the file via LogStore.
 *
 * Usage:  #include "log_file.h"  then  LOGI("tag", "fmt %d", x);
 *
 * Part of APKLive Phase 2. See docs/ARCHITECTURE.md.
 */
#ifndef APKCONTAINER_LOG_FILE_H
#define APKCONTAINER_LOG_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3
};

/* One-time init. Idempotent. Opens (or creates) the current-run log file. */
void log_init(void);

/* Set the directory where logs are written (overrides default). Call before
 * log_init() if you want a custom location. The default is
 *   <Application Support>/APKLive/logs/apklive-<timestamp>.log */
void log_set_dir(const char *abs_path);

/* Returns the absolute path of the current run's log file (valid after log_init).
 * Caller does NOT free. */
const char *log_current_path(void);

/* Core writer. Thread-safe via an internal mutex. */
void log_write(int level, const char *tag, const char *fmt, ...);

/* Flush + close the current run's log. Called at app exit. */
void log_shutdown(void);

/* Rotate: keep the last N run logs, delete older. Called automatically at init. */
void log_rotate(int keep_count);

/* Convenience macros so callers don't repeat the level arg. */
#define LOGD(tag, ...) log_write(LOG_LVL_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) log_write(LOG_LVL_INFO,  tag, __VA_ARGS__)
#define LOGW(tag, ...) log_write(LOG_LVL_WARN,  tag, __VA_ARGS__)
#define LOGE(tag, ...) log_write(LOG_LVL_ERROR, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif
