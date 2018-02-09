#include "../common.h"

void memzero(void* a, size_t n)
{
	char* p = (char*) a;
	char* e = p + n;

	while(p < e) *p++ = 0;
}
