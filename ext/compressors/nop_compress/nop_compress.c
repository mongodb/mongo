#include <errno.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

static int
nop_compress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *, int *);
static int
nop_decompress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *);

static WT_COMPRESSOR nop_compressor = { nop_compress, nop_decompress };

#define	__UNUSED(v)	((void)(v))

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

	__UNUSED(api);
	__UNUSED(config);
	conn = session->connection;

	(void)conn->add_compressor(conn, "nop_compress", &nop_compressor, NULL);
	return (0);
}

/* For OS X */
__attribute__((destructor))
static void _fini(void) {
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
nop_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest, int *compression_failed)
{
	__UNUSED(compressor);
	__UNUSED(session);

	*compression_failed = 0;
        if (dest->size < source->size) {
		*compression_failed = 1;
                return (0);
        }

        memcpy((void *)dest->data, source->data, source->size);
        dest->size = source->size;

        return (0);
}

static int
nop_decompress(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const WT_ITEM *source, WT_ITEM *dest)
{
	__UNUSED(compressor);
	__UNUSED(session);

        if (dest->size < source->size)
                return (ENOMEM);

        memcpy((void *)dest->data, source->data, source->size);
        dest->size = source->size;
        return (0);
}
/* End implementation of WT_COMPRESSOR. */
