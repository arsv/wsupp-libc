#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

typedef unsigned char byte;

#define __unused __attribute__((unused))
#define __packed __attribute__((packed))
#define noreturn __attribute__((noreturn))

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*a))
#define ARRAY_END(a) (a + ARRAY_SIZE(a))

#define PF __attribute__((format(printf,1,2)))

#define STDIN  0
#define STDOUT 1
#define STDERR 2

void warn(const char* fmt, ...) PF;
void fail(const char* fmt, ...) PF noreturn;
void _exit(int) noreturn;

int nonzero(void* a, size_t n);
void memzero(void* a, size_t n);

long writeall(int fd, void* buf, long len);

#endif
