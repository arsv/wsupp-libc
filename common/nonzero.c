#include "../common.h"

int nonzero(void* a, size_t n)
{
	char* p = a;
	char* e = a + n;

	for(; p < e; p++)
		if(*p) return 1;

	return 0;
}
