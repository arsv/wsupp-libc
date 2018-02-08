#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include "../common.h"

extern const char errtag[];

void warn(const char* fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s: ", errtag);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void fail(const char* fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s: ", errtag);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	_exit(0xFF);
}
