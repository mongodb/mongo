#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include <wiredtiger.h>
#include <stdarg.h>

void check(const char *fmt, ...)
{
	char buf[200], *end, *p;
	va_list ap;
	size_t len;

	va_start(ap, fmt);
	len = wiredtiger_struct_sizev(fmt, ap);
	va_end(ap);

	assert(len < sizeof buf);
	
	va_start(ap, fmt);
	assert(wiredtiger_struct_packv(buf, sizeof buf, fmt, ap) == 0);
	va_end(ap);

	printf("%s ", fmt);
	for (p = buf, end = p + len; p < end; p++)
		printf("%02x", *p & 0xff);
	printf("\n");
}

int main() {
	check("iii", 0, 101, -99);
	check("3i", 0, 101, -99);
	check("iS", 42, "forty two");
#if 0
	/* TODO: need a WT_ITEM */
	check("u", r"\x42" * 20)
	check("uu", r"\x42" * 10, r"\x42" * 10)
#endif
	return (0);
}
