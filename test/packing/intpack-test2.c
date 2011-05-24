#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include <wt_internal.h>
#include "intpack.i"

int main() {
	uint8_t buf[10], *p, *end;
	int64_t i;

	for (i = 1; i < 1LL << 60; i <<= 1) {
		end = buf;
		__wt_vpack_uint(NULL, &end, sizeof buf, i);
		printf("%lld ", i);
		for (p = buf; p < end; p++)
			printf("%02x", *p);
		printf("\n");

		end = buf;
		__wt_vpack_int(NULL, &end, sizeof buf, -i);
		printf("%lld ", -i);
		for (p = buf; p < end; p++)
			printf("%02x", *p);
		printf("\n");
	}

	return (0);
}
