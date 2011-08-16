#include <wiredtiger.h>
#include <string.h>
#include <errno.h>

static int
nop_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest);
	
static int
nop_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest);

static WT_COMPRESSOR nop_compressor = { nop_compress, nop_decompress };

int wiredtiger_extension_init(WT_CONNECTION *conn,
    WT_EXTENSION_API *api, const char *config)
{
        (void)api;
        (void)config;
        //fprintf(stderr, "nop_compress: extension_init called!\n");
	conn->add_compressor(conn, "nop_compress", &nop_compressor, NULL);
        return (0);
}

/* For OS X */
__attribute__((destructor))
static void _fini(void) {
        //fprintf(stderr, "nop_compress: _fini called!\n");
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
nop_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest)
{
        /* Unused parameters */
        (void)compressor;
        (void)session;

        //fprintf(stderr, "nop_compress: compress called!\n");
        if (dest->size < source->size) {
                dest->size = source->size;
                return (ENOMEM);
        }
        memcpy((void *)dest->data, source->data, source->size);
        dest->size = source->size;
        return (0);
}

static int
nop_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest)
{
        /* Unused parameters */
        (void)compressor;
        (void)session;

        //fprintf(stderr, "nop_compress: decompress called!\n");
        if (dest->size < source->size) {
                dest->size = source->size;
                return (ENOMEM);
        }
        memcpy((void *)dest->data, source->data, source->size);
        dest->size = source->size;
        return (0);
}
/* End implementation of WT_COMPRESSOR. */
