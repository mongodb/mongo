#include <wt_int.h>

#include <string.h>

int main(int argc, char *argv[])
{
	int ret;
	WT_CONFIG c;
	WT_CONFIG_ITEM k, v;
	const char *cstr = "create,cachesize=10MB";

	config_init(&c, cstr, strlen(cstr));
	while ((ret = config_next(&c, &k, &v)) == 0) {
		printf("Got key '%.*s', value '%.*s'\n",
		    (int)k.len, k.str, (int)v.len, v.str);
	}

	printf("Last call to config_next failed with %d\n", ret);

	return (0);
}
