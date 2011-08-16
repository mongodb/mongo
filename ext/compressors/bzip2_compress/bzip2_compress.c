#include <string.h>
#include <errno.h>
#include <wiredtiger_ext.h>
#include <bzlib.h>

WT_EXTENSION_API *wt_api;

static int
bzip2_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest);
	
static int
bzip2_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest);

static WT_COMPRESSOR bzip2_compressor = { bzip2_compress, bzip2_decompress };

/* between 0-4: set the amount of verbosity to stderr */
static const int bz_verbosity = 0;

/* between 1-9: set the block size to 100k x this number (compression only) */
static const int bz_blocksize100k = 1;

/* between 0-250: workFactor: see bzip2 manual.  0 is a reasonable default (compression only) */
static const int bz_workfactor = 0;

/* if nonzero, decompress using less memory, but slower (decompression only) */
static const int bz_small = 0;

int wiredtiger_extension_init(WT_CONNECTION *conn, WT_EXTENSION_API *api, const char *config)
{
	wt_api = api;

        (void)config;
        /* fprintf(stderr, "bzip2_compress: extension_init called!\n"); */
	conn->add_compressor(conn, "bzip2_compress", &bzip2_compressor, NULL);
        return (0);
}

/* For OS X */
__attribute__((destructor))
static void _fini(void) {
        /* fprintf(stderr, "bzip2_compress: _fini called!\n"); */
}

/* Convert a Bzip2 library return code to WT error code */
static int
bzip2_convert_error(int bzret)
{
	int wtret;

	/* Specific error numbers shown via fprintf can be decoded in bzlib.h */
	switch (bzret) {
	case BZ_OUTBUFF_FULL:
		wtret = WT_TOOSMALL;
		break;
	case BZ_CONFIG_ERROR:
	case BZ_PARAM_ERROR:
		fprintf(stderr, "bzip2_compress: compress internal config error %d\n", bzret);
		wtret = WT_ERROR;
		break;
	case BZ_MEM_ERROR:
		wtret = ENOMEM;
		break;
	case BZ_DATA_ERROR:
	case BZ_DATA_ERROR_MAGIC:
	case BZ_UNEXPECTED_EOF:
		fprintf(stderr, "bzip2_compress: compress internal config error %d\n", bzret);
		wtret = WT_ERROR;
		break;
	default:
		fprintf(stderr, "bzip2_compress: compress error %d\n", bzret);
		wtret = WT_ERROR;
		break;
	}
	return (wtret);
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
bzip2_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest)
{
        /* Unused parameters */
        (void)compressor;
        (void)session;
	int ret;
	int bzret;
	unsigned int destlen;

#ifdef TRACE_BZIP_CALLS
	static int ncalls = -1;
        if (++ncalls % 1000 == 0)
		fprintf(stderr, "bzip2_compress: compress called %d times\n", ncalls);
#endif

	destlen = dest->size;
	bzret = BZ2_bzBuffToBuffCompress((char *)dest->data, &destlen,
	    (char *)source->data, source->size,
	    bz_blocksize100k, bz_verbosity, bz_workfactor);

	if (bzret == BZ_OK) {
		dest->size = destlen;
		ret = 0;
	}
	else
		ret = bzip2_convert_error(bzret);

        return (ret);
}

static int
bzip2_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest)
{
        /* Unused parameters */
        (void)compressor;
        (void)session;
	int ret;
	int bzret;
	unsigned int destlen;

#ifdef TRACE_BZIP_CALLS
	static int ncalls = -1;
        if (++ncalls % 1000 == 0)
		fprintf(stderr, "bzip2_compress: decompress called %d times\n", ncalls);
#endif

	destlen = dest->size;
	bzret = BZ2_bzBuffToBuffDecompress((char *)dest->data, &destlen,
	    (char *)source->data, (unsigned int)source->size,
	    (int)bz_small, (int)bz_verbosity);

	if (bzret == BZ_OK) {
		dest->size = destlen;
		ret = 0;
	}
	else
		ret = bzip2_convert_error(bzret);

        return (ret);
}
/* End implementation of WT_COMPRESSOR. */
