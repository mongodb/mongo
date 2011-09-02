#include <bzlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

static int
bzip2_compress(
    WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *, int *);
static int
bzip2_decompress(WT_COMPRESSOR *, WT_SESSION *, const WT_ITEM *, WT_ITEM *);

static WT_COMPRESSOR bzip2_compressor = { bzip2_compress, bzip2_decompress };

#define	__UNUSED(v)	((void)(v))

/* between 0-4: set the amount of verbosity to stderr */
static int bz_verbosity = 0;

/* between 1-9: set the block size to 100k x this number (compression only) */
static int bz_blocksize100k = 1;

/*
 * between 0-250: workFactor: see bzip2 manual.  0 is a reasonable default
 * (compression only)
 */
static int bz_workfactor = 0;

/* if nonzero, decompress using less memory, but slower (decompression only) */
static int bz_small = 0;

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

        __UNUSED(config);

	wt_api = api;
	conn = session->connection;

	return (conn->add_compressor(
	    conn, "bzip2_compress", &bzip2_compressor, NULL));
}

/* Bzip2 WT_COMPRESSOR implementation for WT_CONNECTION::add_compressor. */
/*
 * bzip2_error --
 *	Output an error message, and return a standard error code.
 */
static int
bzip2_error(WT_SESSION *session, const char *call, int bzret)
{
	const char *msg;

	switch (bzret) {
	case BZ_MEM_ERROR:
		msg = "BZ_MEM_ERROR";
		break;
	case BZ_OUTBUFF_FULL:
		msg = "BZ_OUTBUFF_FULL";
		break;
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

	wiredtiger_err_printf(
	    session, "bzip2 error: %s: %s: %d", call, msg, bzret);
	return (WT_ERROR);
}

static void *
bzalloc(void *cookie, int number, int size)
{
	return (wiredtiger_scr_alloc(cookie, (size_t)number * size));
}

static void
bzfree(void *cookie, void *p)
{
	wiredtiger_scr_free(cookie, p);
}

static int
bzip2_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *src, WT_ITEM *dst, int *compression_failed)
{
	bz_stream bz;
	int ret;

	__UNUSED(compressor);

	memset(&bz, 0, sizeof(bz));
	bz.bzalloc = bzalloc;
	bz.bzfree = bzfree;
	bz.opaque = session;

	if ((ret = BZ2_bzCompressInit(&bz,
	    bz_blocksize100k, bz_verbosity, bz_workfactor)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzCompressInit", ret));

	bz.next_in = (char *)src->data;
	bz.avail_in = src->size;
	bz.next_out = (char *)dst->data;
	bz.avail_out = dst->size;
	if ((ret = BZ2_bzCompress(&bz, BZ_FINISH)) == BZ_STREAM_END) {
		*compression_failed = 0;
		dst->size -= bz.avail_out;
	} else
		*compression_failed = 1;

	if ((ret = BZ2_bzCompressEnd(&bz)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzCompressEnd", ret));

	return (0);
}

static int
bzip2_decompress(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const WT_ITEM *src, WT_ITEM *dst)
{
	bz_stream bz;
	int ret, tret;

	__UNUSED(compressor);

	memset(&bz, 0, sizeof(bz));
	bz.bzalloc = bzalloc;
	bz.bzfree = bzfree;
	bz.opaque = session;

	if ((ret = BZ2_bzDecompressInit(&bz, bz_small, bz_verbosity)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzDecompressInit", ret));

	bz.next_in = (char *)src->data;
	bz.avail_in = src->size;
	bz.next_out = (char *)dst->data;
	bz.avail_out = dst->size;
	if ((ret = BZ2_bzDecompress(&bz)) == BZ_STREAM_END) {
		dst->size -= bz.avail_out;
		ret = 0;
	} else
		bzip2_error(session, "BZ2_bzDecompress", ret);

	if ((tret = BZ2_bzDecompressEnd(&bz)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzDecompressEnd", tret));

	return (ret == 0 ?
	    0 : bzip2_error(session, "BZ2_bzDecompressEnd", ret));
}
/* End Bzip2 WT_COMPRESSOR implementation for WT_CONNECTION::add_compressor. */
