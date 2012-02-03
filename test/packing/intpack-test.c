#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include <wt_internal.h>
#include "intpack.i"

int main() {
	uint8_t buf[10], *p;
	uint64_t r, r2, ncalls;
	int i, s;

	ncalls = 0;

	for (i = 0; i < 10000000; i++) {
		for (s = 0; s < 50; s += 5) {
			++ncalls;
			r = 1 << s;

#if 1
			p = buf;
			__wt_vpack_uint(NULL, &p, sizeof buf, r);
			p = buf;
			__wt_vunpack_uint(NULL, &p, sizeof buf, &r2);
#else
			/*
			 * Note: use memmove for comparison because GCC does
			 * aggressive optimization of memcpy and it's difficult
			 * to measure anything.
			 */
			p = buf;
			memmove(p, &r, sizeof r);
			p = buf;
			memmove(&r2, p, sizeof r2);
#endif
			if (r != r2) {
				fprintf(stderr, "mismatch!\n");
				break;
			}
		}
	}

	printf("Number of calls: %llu\n", (unsigned long long)ncalls);

	return (0);
}
