/* execinfo.h — no backtrace() writer on Orbis; the engine's crash handler only
 * logs and exits there. */
#ifndef PS4_EXECINFO_H
#define PS4_EXECINFO_H
static inline int backtrace(void **buffer, int size) { (void)buffer; (void)size; return 0; }
static inline char **backtrace_symbols(void *const *buffer, int size) {
    (void)buffer; (void)size; return (char **)0;
}
static inline void backtrace_symbols_fd(void *const *buffer, int size, int fd) {
    (void)buffer; (void)size; (void)fd;
}
#endif
