/*
 * Host-test stub for <zephyr/logging/log.h>.
 *
 * The log macros still EVALUATE their arguments (so an unused-variable warning
 * in the production source would still show up) but print nothing, keeping the
 * test output clean.
 */
#pragma once

static inline void torabo_test_log_sink(const char *fmt, ...) { (void)fmt; }

/* Declared, never defined: the trailing `;` at the call site terminates it. */
#define LOG_MODULE_REGISTER(...) extern int torabo_test_log_module_dummy
#define LOG_MODULE_DECLARE(...) extern int torabo_test_log_module_dummy

#define LOG_ERR(...) torabo_test_log_sink(__VA_ARGS__)
#define LOG_WRN(...) torabo_test_log_sink(__VA_ARGS__)
#define LOG_INF(...) torabo_test_log_sink(__VA_ARGS__)
#define LOG_DBG(...) torabo_test_log_sink(__VA_ARGS__)
#define LOG_HEXDUMP_DBG(...) ((void)0)
