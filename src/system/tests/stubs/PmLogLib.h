/* Minimal PmLogLib stand-in so the system module compiles on a host. */
#ifndef PMLOGLIB_STUB_H
#define PMLOGLIB_STUB_H
#include <stdio.h>
typedef void *PmLogContext;
#define PMLOGKS(k, v) k, v
#define PMLOGKFV(k, f, v) k, v
#define PmLogCritical(ctx, msgid, kv, ...) fprintf(stderr, "[%s] ", msgid), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)
#define PmLogError(ctx, msgid, kv, ...)    fprintf(stderr, "[%s] ", msgid), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)
#define PmLogWarning(ctx, msgid, kv, ...)  fprintf(stderr, "[%s] ", msgid), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)
#define PmLogInfo(ctx, msgid, kv, ...)     fprintf(stderr, "[%s] ", msgid), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)
#define PmLogDebug(ctx, ...)               ((void)0)
#endif
