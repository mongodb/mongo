#include <errno.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

static int
nop_compress(WT_COMPRESSOR *, WT_SESSION *,
    uint8_t *, size_t, uint8_t *, size_t, size_t *, int *);
static int
nop_decompress(WT_COMPRESSOR *, WT_SESSION *,
    uint8_t *, size_t, uint8_t *, size_t, size_t *);

static WT_COMPRESSOR nop_compressor = { nop_compress, nop_decompress, NULL };

#define	__UNUSED(v)	((void)(v))

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

	__UNUSED(config);

	wt_api = api;
	conn = session->connection;

	return (
	    conn->add_compressor(conn, "nop_compress", &nop_compressor, NULL));
}

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
nop_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	__UNUSED(compressor);
	__UNUSED(session);

	*compression_failed = 0;
	if (dst_len < src_len) {
		*compression_failed = 1;
		return (0);
	}

	memcpy(dst, src, src_len);
	*result_lenp = src_len;

	return (0);
}

static int
nop_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	__UNUSED(compressor);
	__UNUSED(session);

	if (dst_len < src_len)
		return (ENOMEM);

	memcpy(dst, src, src_len);
	*result_lenp = src_len;
	return (0);
}
/* End implementation of WT_COMPRESSOR. */
