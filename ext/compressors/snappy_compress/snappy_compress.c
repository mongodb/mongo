#include <errno.h>
#include <snappy-c.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

static int
wt_snappy_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest, int *);
	
static int
wt_snappy_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *source, WT_ITEM *dest);

static WT_COMPRESSOR wt_snappy_compressor = {
    wt_snappy_compress, wt_snappy_decompress };

#define	__UNUSED(v)	((void)(v))

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

	__UNUSED(config);

	wt_api = api;
	conn = session->connection;

	return (conn->add_compressor(
            conn, "snappy_compress", &wt_snappy_compressor, NULL));
}

/*
 * wt_snappy_error --
 *	Output an error message, and return a standard error code.
 */
static int
wt_snappy_error(WT_SESSION *session, const char *call, snappy_status snret)
{
	const char *msg;

	switch (snret) {
	case SNAPPY_BUFFER_TOO_SMALL:
		msg = "SNAPPY_BUFFER_TOO_SMALL";
		break;
	case SNAPPY_INVALID_INPUT:
		msg = "SNAPPY_INVALID_INPUT";
		break;
	default:
		msg = "unknown error";
		break;
	}

	wiredtiger_err_printf(
	    session, "snappy error: %s: %s: %d", call, msg, snret);
	return (WT_ERROR);
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
wt_snappy_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *src, WT_ITEM *dst, int *compression_failed)
{
	snappy_status snret;
	int ret;
	size_t snaplen;
	void *buf;
	char *snapbuf;
	unsigned char *destp;

        __UNUSED(compressor);

	/* Snappy requires that the dest buffer be somewhat larger
	 * than the source, but WT hands us a a dest buffer the same
	 * size.  This leads to an unfortunate extra copy, but it
	 * can't be helped with the current WT interface and snappy
	 * API.  We use a scratch buffer for temp storage, it should
	 * be much faster than malloc.
	 */
	snaplen = snappy_max_compressed_length(src->size);
	buf = wiredtiger_scr_alloc(session, snaplen);
	snapbuf = (char *)buf;

	/* snaplen is an input and an output arg. */
	snret = snappy_compress((char *)src->data, src->size,
	    snapbuf, &snaplen);

	if (snret == SNAPPY_OK) {
		/* On decompression, snappy requires the exact
		 * compressed buffer size (the current value of
		 * snaplen).  WT does not preserve that, it rounds
		 * the length up.  So we save the snaplen at the
		 * beginning of the destination buffer.
		 */
		if (snaplen + sizeof(size_t) < dst->size) {
			destp = (unsigned char *)dst->data;
			*(size_t *)destp = snaplen;
			destp += sizeof(size_t);
			memcpy(destp, snapbuf, snaplen);
			dst->size = (uint32_t)(snaplen + sizeof(size_t));
			*compression_failed  = 0;
		}
		else
			/* The compressor failed to produce a smaller result. */
			*compression_failed = 1;

		ret = 0;
	}
	else
		ret = wt_snappy_error(session, "snappy_compress", snret);

	wiredtiger_scr_free(session, buf);

        return (ret);
}

static int
wt_snappy_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    const WT_ITEM *src, WT_ITEM *dst)
{
	snappy_status snret;
	int ret;
	size_t dstlen;
	size_t snaplen;

        __UNUSED(compressor);

	/* retrieve the saved length */
	snaplen = *((size_t *)src->data);
	if (snaplen + sizeof(size_t) > src->size) {
		wiredtiger_err_printf(
		    session,
		    "wt_snappy_decompress: stored size exceeds buf size");
		return (WT_ERROR);
	}
	dstlen = dst->size;
	snret = snappy_uncompress(((char *)src->data) + sizeof(size_t), snaplen,
	    (char *)dst->data, &dstlen);

	if (snret == SNAPPY_OK) {
		dst->size = (uint32_t)dstlen;
		ret = 0;
	}
	else
		ret = wt_snappy_error(session, "snappy_uncompress", snret);

        return (ret);
}
/* End implementation of WT_COMPRESSOR. */
