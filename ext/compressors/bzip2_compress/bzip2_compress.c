#include <bzlib.h>
#include <errno.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

static int
bzip2_compress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *);
static int
bzip2_decompress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *);

static WT_COMPRESSOR bzip2_compressor = { bzip2_compress, bzip2_decompress };

#define	__UNUSED(v)	((void)(v))

/* between 0-4: set the amount of verbosity to stderr */
static const int bz_verbosity = 0;

/* between 1-9: set the block size to 100k x this number (compression only) */
static const int bz_blocksize100k = 1;

/*
 * between 0-250: workFactor: see bzip2 manual.  0 is a reasonable default
 * (compression only)
 */
static const int bz_workfactor = 0;

/* if nonzero, decompress using less memory, but slower (decompression only) */
static const int bz_small = 0;

int
wiredtiger_extension_init(
    WT_CONNECTION *conn, WT_EXTENSION_API *api, const char *config)
{
	wt_api = api;
        __UNUSED(config);

	(void)conn->add_compressor(
	    conn, "bzip2_compress", &bzip2_compressor, NULL);
	return (0);
}

/* For OS X */
__attribute__((destructor))
static void _fini(void) {
}

/* Convert a Bzip2 library return code to WT error code */
static int
bzip2_convert_error(WT_SESSION *wt_session, int bzret)
{
	const char *msg;

	switch (bzret) {		/* Some errors are anticipated */
	case BZ_MEM_ERROR:
		return (ENOMEM);
	case BZ_OUTBUFF_FULL:
		return (WT_TOOSMALL);
	}

	switch (bzret) {		/* Some errors are unexpected */
	case BZ_SEQUENCE_ERROR:
		msg = "BZ_SEQUENCE_ERROR";
		break;
	case BZ_PARAM_ERROR:
		msg = "BZ_PARAM_ERROR";
		break;
	case BZ_DATA_ERROR:
		msg = "BZ_DATA_ERROR";
		break;
	case BZ_DATA_ERROR_MAGIC:
		msg = "BZ_DATA_ERROR_MAGIC";
		break;
	case BZ_IO_ERROR:
		msg = "BZ_IO_ERROR";
		break;
	case BZ_UNEXPECTED_EOF:
		msg = "BZ_UNEXPECTED_EOF";
		break;
	case BZ_CONFIG_ERROR:
		msg = "BZ_CONFIG_ERROR";
		break;
	default:
		msg = "unknown error";
		break;
	}

	/*
	 * XXX
	 * This needs to get pushed back to the session handle somehow.
	 */
	wiredtiger_err_printf(wt_session, "bzip2 error: %s: %d\n", msg, bzret);
	return (WT_ERROR);
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
bzip2_compress(WT_COMPRESSOR *compressor,
    WT_SESSION *wt_session, const WT_ITEM *source, WT_ITEM *dest)
{
	u_int destlen;
	int bzret;

	__UNUSED(compressor);

	destlen = dest->size;
	bzret = BZ2_bzBuffToBuffCompress((char *)dest->data, &destlen,
	    (char *)source->data, source->size,
	    bz_blocksize100k, bz_verbosity, bz_workfactor);

	if (bzret == BZ_OK) {
		dest->size = destlen;
		return (0);
	}
	return (bzip2_convert_error(wt_session, bzret));
}

static int
bzip2_decompress(WT_COMPRESSOR *compressor,
    WT_SESSION *wt_session, const WT_ITEM *source, WT_ITEM *dest)
{
	int bzret;
	u_int destlen;

	__UNUSED(compressor);

	destlen = dest->size;
	bzret = BZ2_bzBuffToBuffDecompress((char *)dest->data, &destlen,
	    (char *)source->data, (u_int)source->size,
	    (int)bz_small, (int)bz_verbosity);

	if (bzret == BZ_OK) {
		dest->size = destlen;
		return (0);
	}
	return (bzip2_convert_error(wt_session, bzret));
}
/* End implementation of WT_COMPRESSOR. */
