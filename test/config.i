%module config

%{
#include <wt_int.h>
%}

typedef struct WT_CONFIG WT_CONFIG;
typedef struct WT_CONFIG_ITEM WT_CONFIG_ITEM;

struct WT_CONFIG
{
	char *orig;
	char *end;
	char *cur;

        int depth, top;
	void **go;
};

struct WT_CONFIG_ITEM
{
	char *str;
	size_t len;
	uint64_t val;
	enum { ITEM_STRING, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};

/*
 * XXX: leak, but the string needs to last beyond the call to config_init.
 * It should be freed when the WT_CONFIG object is deleted.
 */
%typemap(in) char *confstr {
        $1 = strdup(PyString_AsString($input));
}

int config_init(WT_CONFIG *conf, char *confstr, int len);
int config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value);
