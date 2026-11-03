/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_TEST_SYS_SYSTIME_H
#define SALSA64_TEST_SYS_SYSTIME_H
#include <stdint.h>
static inline uint64_t sysGetSystemTime(void) { return 0; }
static inline void sysUsleep(uint64_t us) { (void)us; }
#endif
