#include <errno.h>
#include <string.h>

#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

#define __UNUSED(v)     ((void)(v))

static int
collate_reverse(WT_COLLATOR *collator, WT_SESSION *session,
    const WT_ITEM *k1, const WT_ITEM *k2, int *cmp)
{
        size_t len;

        __UNUSED(collator);
        __UNUSED(session);

        len = (k1->size < k2->size) ? k1->size : k2->size;
        if ((*cmp = memcmp(k2->data, k1->data, len)) == 0)
                *cmp = ((int)k1->size - (int)k2->size);
        return (0);
}

static WT_COLLATOR reverse_collator = { collate_reverse };

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
        WT_CONNECTION *conn;

        __UNUSED(config);

        wt_api = api;
        conn = session->connection;

        return (conn->add_collator(conn, "reverse", &reverse_collator, NULL));
}
