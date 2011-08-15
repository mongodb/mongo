#include <errno.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#define	WT_UNUSED(v)	((void)(v))

static int
nop_compress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *);
static int
nop_decompress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *);

static WT_COMPRESSOR nop_compressor = { nop_compress, nop_decompress };

int
wiredtiger_extension_init(
    WT_CONNECTION *conn, WT_EXTENSION_API *api, const char *config)
{
	WT_UNUSED(api);
	WT_UNUSED(config);

	(void)conn->add_compressor(conn, "nop_compress", &nop_compressor, NULL);
	return (0);
}

/* For OS X */
__attribute__((destructor))
static void _fini(void) {
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
nop_compress(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const WT_ITEM *source, WT_ITEM *dest)
{
	WT_UNUSED(compressor);
	WT_UNUSED(session);

        if (dest->size < source->size) {
                dest->size = source->size;
                return (ENOMEM);
        }

        memcpy((void *)dest->data, source->data, source->size);
        dest->size = source->size;

        return (0);
}

static int
nop_decompress(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const WT_ITEM *source, WT_ITEM *dest)
{
	WT_UNUSED(compressor);
	WT_UNUSED(session);

        if (dest->size < source->size) {
                dest->size = source->size;
                return (ENOMEM);
        }

        memcpy((void *)dest->data, source->data, source->size);
        dest->size = source->size;
        return (0);
}
/* End implementation of WT_COMPRESSOR. */
