/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_schema_project_in --
 *	Given list of cursors and a projection, read columns from the
 *	application into the dependent cursors.
 */
int
__wt_schema_project_in(WT_SESSION_IMPL *session,
    WT_CURSOR **cp, const char *proj_arg, va_list ap)
{
	WT_BUF *buf;
	WT_CURSOR *c;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	char *proj;
	uint8_t *p, *end;
	size_t len;
	uint32_t arg, offset;

	/* Reset any of the buffers we will be setting. */
	for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);
		if (*proj == WT_PROJ_KEY) {
			c = cp[arg];
			WT_RET(__wt_buf_init(session, &c->key, 0));
		} else if (*proj == WT_PROJ_VALUE) {
			c = cp[arg];
			WT_RET(__wt_buf_init(session, &c->value, 0));
		}
	}

	for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);

		switch (*proj) {
		case WT_PROJ_KEY:
			c = cp[arg];
			WT_RET(__pack_init(session, &pack, c->key_format));
			buf = &c->key;
			p = (uint8_t *)buf->data;
			end = p + buf->size;
			continue;

		case WT_PROJ_VALUE:
			c = cp[arg];
			WT_RET(__pack_init(session, &pack, c->value_format));
			buf = &c->value;
			p = (uint8_t *)buf->data;
			end = p + buf->size;
			continue;
		}

		/*
		 * Otherwise, the argument is a count, where a missing
		 * count means a count of 1.
		 */
		for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
			switch (*proj) {
			case WT_PROJ_NEXT:
			case WT_PROJ_SKIP:
				WT_RET(__pack_next(&pack, &pv));
				if (*proj == WT_PROJ_SKIP) {
					/*
					 * A nasty case: if we are inserting
					 * out-of-order, we may reach the end
					 * of the data.  That's okay: we want
					 * to append in that case, and we're
					 * positioned to do that.
					 */
					if (p < end)
						WT_RET(__unpack_read(session,
						    &pv, (const uint8_t **)&p,
						    (size_t)(end - p)));
						break;
				}
				WT_PACK_GET(session, pv, ap);
				/* FALLTHROUGH */

			case WT_PROJ_REUSE:
				len = __pack_size(session, &pv);
				offset = (uint32_t)(p - (uint8_t *)buf->data);
				WT_RET(__wt_buf_grow(session,
				    buf, buf->size + len));
				p = (uint8_t *)buf->data + offset;
				end = (uint8_t *)buf->data + buf->size + len;
				/* Make room if we're inserting out-of-order. */
				if (offset < buf->size)
					memmove(p + len, p, buf->size - offset);
				WT_RET(__pack_write(session,
				    &pv, &p, (size_t)(end - p)));
				buf->size += (uint32_t)len;
				break;

			default:
				WT_FAILURE(session,
				    "unexpected projection plan: %c",
				    (int)*proj);
				return (0);
			}
		}
	}

	return (0);
}

/*
 * __wt_schema_project_out --
 *	Given list of cursors and a projection, read columns from the
 *	dependent cursors and return them to the application.
 */
int
__wt_schema_project_out(WT_SESSION_IMPL *session,
    WT_CURSOR **cp, const char *proj_arg, va_list ap)
{
	WT_CURSOR *c;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	char *proj;
	uint8_t *p, *end;
	uint32_t arg;

	for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);

		switch (*proj) {
		case WT_PROJ_KEY:
			c = cp[arg];
			p = (uint8_t *)c->key.data;
			end = p + c->key.size;
			WT_RET(__pack_init(session, &pack, c->key_format));
			continue;

		case WT_PROJ_VALUE:
			c = cp[arg];
			p = (uint8_t *)c->value.data;
			end = p + c->value.size;
			WT_RET(__pack_init(session, &pack, c->value_format));
			continue;
		}

		/*
		 * Otherwise, the argument is a count, where a missing
		 * count means a count of 1.
		 */
		for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
			switch (*proj) {
			case WT_PROJ_NEXT:
			case WT_PROJ_SKIP:
				WT_RET(__pack_next(&pack, &pv));
				WT_RET(__unpack_read(session, &pv,
				    (const uint8_t **)&p, (size_t)(end - p)));
				if (*proj == WT_PROJ_SKIP)
					break;
				WT_UNPACK_PUT(session, pv, ap);
				/* FALLTHROUGH */

			case WT_PROJ_REUSE:
				/* Don't copy out the same value twice. */
				break;
			}
		}
	}

	return (0);
}

/*
 * __wt_schema_project_slice --
 *	Given list of cursors and a projection, read columns from the
 *	a raw buffer.
 */
int
__wt_schema_project_slice(WT_SESSION_IMPL *session,
    WT_CURSOR **cp, const char *proj_arg, const char *vformat, WT_ITEM *value)
{
	WT_BUF *buf;
	WT_CURSOR *c;
	WT_PACK pack, vpack;
	WT_PACK_VALUE pv, vpv;
	char *proj;
	uint8_t *p, *end;
	const uint8_t *vp, *vend;
	size_t len;
	uint32_t arg, offset;

	WT_RET(__pack_init(session, &vpack, vformat));
	vp = (uint8_t *)value->data;
	vend = vp + value->size;

	/* Reset any of the buffers we will be setting. */
	for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);
		if (*proj == WT_PROJ_KEY) {
			c = cp[arg];
			WT_RET(__wt_buf_init(session, &c->key, 0));
		} else if (*proj == WT_PROJ_VALUE) {
			c = cp[arg];
			WT_RET(__wt_buf_init(session, &c->value, 0));
		}
	}

	for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);

		switch (*proj) {
		case WT_PROJ_KEY:
			c = cp[arg];
			WT_RET(__pack_init(session, &pack, c->key_format));
			buf = &c->key;
			p = (uint8_t *)buf->data;
			end = p + buf->size;
			continue;

		case WT_PROJ_VALUE:
			c = cp[arg];
			WT_RET(__pack_init(session, &pack, c->value_format));
			buf = &c->value;
			p = (uint8_t *)buf->data;
			end = p + buf->size;
			continue;
		}

		/*
		 * Otherwise, the argument is a count, where a missing
		 * count means a count of 1.
		 */
		for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
			switch (*proj) {
			case WT_PROJ_NEXT:
			case WT_PROJ_SKIP:
				WT_RET(__pack_next(&pack, &pv));
				if (*proj == WT_PROJ_SKIP) {
					/*
					 * A nasty case: if we are inserting
					 * out-of-order, we may reach the end
					 * of the data.  That's okay: we want
					 * to append in that case, and we're
					 * positioned to do that.
					 */
					if (p < end)
						WT_RET(__unpack_read(session,
						    &pv, (const uint8_t **)&p,
						    (size_t)(end - p)));
						break;
				}
				WT_RET(__pack_next(&vpack, &vpv));
				WT_RET(__unpack_read(session, &vpv,
				    &vp, (size_t)(vend - vp)));
				/* FALLTHROUGH */
			case WT_PROJ_REUSE:
				/*
				 * There is subtlety here: the value format
				 * may not exactly match the cursor's format.
				 * In particular, we need lengths with raw
				 * columns in the middle of a packed struct,
				 * but not if they are at the end of a column.
				 */
				pv.u = vpv.u;

				len = __pack_size(session, &pv);
				offset = (uint32_t)(p - (uint8_t *)buf->data);
				WT_RET(__wt_buf_grow(session,
				    buf, buf->size + len));
				p = (uint8_t *)buf->data + offset;
				end = (uint8_t *)buf->data + buf->size + len;
				/* Make room if we're inserting out-of-order. */
				if (offset < buf->size)
					memmove(p + len, p, buf->size - offset);
				WT_RET(__pack_write(session,
				    &pv, &p, (size_t)(end - p)));
				buf->size += (uint32_t)len;
				break;
			default:
				WT_FAILURE(session,
				    "unexpected projection plan: %c",
				    (int)*proj);
			}
		}
	}

	return (0);
}

/*
 * __wt_schema_project_merge --
 *	Given list of cursors and a projection, build a buffer containing the
 *	column values read from the cursors.
 */
int
__wt_schema_project_merge(WT_SESSION_IMPL *session,
    WT_CURSOR **cp, const char *proj_arg, const char *vformat, WT_BUF *value)
{
	WT_BUF *buf;
	WT_CURSOR *c;
	WT_PACK pack, vpack;
	WT_PACK_VALUE pv, vpv;
	char *proj;
	uint8_t *p, *end, *vp;
	size_t len;
	uint32_t arg;

	WT_RET(__wt_buf_init(session, value, 0));
	WT_RET(__pack_init(session, &vpack, vformat));

	for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);

		switch (*proj) {
		case WT_PROJ_KEY:
			c = cp[arg];
			WT_RET(__pack_init(session, &pack, c->key_format));
			buf = &c->key;
			p = (uint8_t *)buf->data;
			end = p + buf->size;
			continue;

		case WT_PROJ_VALUE:
			c = cp[arg];
			WT_RET(__pack_init(session, &pack, c->value_format));
			buf = &c->value;
			p = (uint8_t *)buf->data;
			end = p + buf->size;
			continue;
		}

		/*
		 * Otherwise, the argument is a count, where a missing
		 * count means a count of 1.
		 */
		for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
			switch (*proj) {
			case WT_PROJ_NEXT:
			case WT_PROJ_SKIP:
				WT_RET(__pack_next(&pack, &pv));
				WT_RET(__unpack_read(session, &pv,
				    (const uint8_t **)&p, (size_t)(end - p)));
				if (*proj == WT_PROJ_SKIP)
					break;

				WT_RET(__pack_next(&vpack, &vpv));
				vpv.u = pv.u;
				len = __pack_size(session, &vpv);
				WT_RET(__wt_buf_grow(session,
				    value, value->size + len));
				vp = (uint8_t *)value->data + value->size;
				WT_RET(__pack_write(session, &vpv, &vp, len));
				value->size += (uint32_t)len;
				/* FALLTHROUGH */

			case WT_PROJ_REUSE:
				/* Don't copy the same value twice. */
				break;
			}
		}
	}

	return (0);
}
