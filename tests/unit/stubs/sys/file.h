/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sustitutos mínimos del sistema de archivos para probar el cierre de estados. */
#ifndef SALSA64_TEST_SYS_FILE_H
#define SALSA64_TEST_SYS_FILE_H
#include <stdint.h>
typedef int32_t s32;
typedef struct { uint64_t st_size; } sysFSStat;
#define SYS_O_RDONLY 0
static inline int sysLv2FsRead(int fd, void *buf, uint64_t n, uint64_t *done)
{ (void)fd; (void)buf; (void)n; *done = 0; return -1; }
static inline int sysLv2FsMkdir(const char *path, int mode)
{ (void)path; (void)mode; return -1; }
static inline int sysLv2FsStat(const char *path, sysFSStat *st)
{ (void)path; (void)st; return -1; }
static inline int sysLv2FsOpen(const char *path, int mode, s32 *fd, int perm, void *arg, uint64_t n)
{ (void)path; (void)mode; (void)fd; (void)perm; (void)arg; (void)n; return -1; }
static inline int sysLv2FsClose(int fd) { (void)fd; return 0; }
#endif
