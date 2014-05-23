/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __json_unpack_put --
 *	Calculate the size of a packed byte string as formatted for JSON.
 */
static size_t
__json_unpack_put(WT_SESSION_IMPL *session, void *voidpv,
    u_char *buf, size_t bufsz, WT_CONFIG_ITEM *name)
{
	WT_PACK_VALUE *pv;
	const char *p, *end;
	size_t s, n;

	pv = (WT_PACK_VALUE *)voidpv;
	s = (size_t)snprintf((char *)buf, bufsz, "\"%.*s\" : ",
	    (int)name->len, name->str);
	if (s <= bufsz) {
		bufsz -= s;
		buf += s;
	}
	else
		bufsz = 0;

	switch (pv->type) {
	case 'x':
		return (0);
	case 's':
	case 'S':
		/* Account for '"' quote in front and back. */
		s += 2;
		p = (const char *)pv->u.s;
		if (bufsz > 0) {
			*buf++ = '"';
			bufsz--;
		}
		if (pv->type == 's' || pv->havesize) {
			end = p + pv->size;
			for (; p < end; p++) {
				n = __wt_json_unpack_char(*p, buf, bufsz, 0);
				if (n > bufsz)
					bufsz = 0;
				else {
					bufsz -= n;
					buf += n;
				}
				s += n;
			}
		} else
			for (; *p; p++) {
				n = __wt_json_unpack_char(*p, buf, bufsz, 0);
				if (n > bufsz)
					bufsz = 0;
				else {
					bufsz -= n;
					buf += n;
				}
				s += n;
			}
		if (bufsz > 0)
			*buf++ = '"';
		return (s);
	case 'U':
	case 'u':
		s += 2;
		p = (const char *)pv->u.item.data;
		end = p + pv->u.item.size;
		if (bufsz > 0) {
			*buf++ = '"';
			bufsz--;
		}
		for (; p < end; p++) {
			n = __wt_json_unpack_char(*p, buf, bufsz, 1);
			if (n > bufsz)
				bufsz = 0;
			else {
				bufsz -= n;
				buf += n;
			}
			s += n;
		}
		if (bufsz > 0)
			*buf++ = '"';
		return (s);
	case 'b':
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		return (s +
		    (size_t)snprintf((char *)buf, bufsz, "%" PRId64, pv->u.i));
	case 'B':
	case 't':
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'r':
	case 'R':
		return (s +
		    (size_t)snprintf((char *)buf, bufsz, "%" PRId64, pv->u.u));
	}
	__wt_err(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
	return ((size_t)-1);
}

/*
 * __json_struct_size --
 *	Calculate the size of a packed byte string as formatted for JSON.
 */
static inline int
__json_struct_size(WT_SESSION_IMPL *session, const void *buffer,
    size_t size, const char *fmt, WT_CONFIG_ITEM *names, int iskey,
    size_t *presult)
{
	WT_CONFIG_ITEM name;
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_PACK pack;
	WT_PACK_NAME packname;
	const uint8_t *p, *end;
	size_t result;
	int needcr;

	p = buffer;
	end = p + size;
	result = 0;
	needcr = 0;

	WT_RET(__pack_name_init(session, names, iskey, &packname));
	WT_RET(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0) {
		if (needcr)
			result += 2;
		needcr = 1;
		WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_RET(__pack_name_next(&packname, &name));
		result += __json_unpack_put(session, &pv, NULL, 0, &name);
	}
	if (ret == WT_NOTFOUND)
		ret = 0;

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	*presult = result;
	return (ret);
}

/*
 * __json_struct_unpackv --
 *	Unpack a byte string to JSON (va_list version).
 */
static inline int
__json_struct_unpackv(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, WT_CONFIG_ITEM *names,
    u_char *jbuf, size_t jbufsize, int iskey, va_list ap)
{
	WT_CONFIG_ITEM name;
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_PACK pack;
	WT_PACK_NAME packname;
	int needcr;
	size_t jsize;
	const uint8_t *p, *end;

	p = buffer;
	end = p + size;
	needcr = 0;

	/* Unpacking a cursor marked as json implies a single arg. */
	*va_arg(ap, const char **) = (char *)jbuf;

	WT_RET(__pack_name_init(session, names, iskey, &packname));
	WT_RET(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0) {
		if (needcr) {
			WT_ASSERT(session, jbufsize >= 3);
			strncat((char *)jbuf, ",\n", jbufsize);
			jbuf += 2;
			jbufsize -= 2;
		}
		needcr = 1;
		WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_RET(__pack_name_next(&packname, &name));
		jsize = __json_unpack_put(session,
		    (u_char *)&pv, jbuf, jbufsize, &name);
		WT_ASSERT(session, jsize <= jbufsize);
		jbuf += jsize;
		jbufsize -= jsize;
	}
	if (ret == WT_NOTFOUND)
		ret = 0;

	/* Be paranoid - __unpack_read should never overflow. */
	WT_ASSERT(session, p <= end);

	WT_ASSERT(session, jbufsize == 1);

	return (ret);
}

/*
 * __wt_json_alloc_unpack --
 *	Allocate space for, and unpack an entry into JSON format.
 */
int
__wt_json_alloc_unpack(WT_SESSION_IMPL *session, const void *buffer,
    size_t size, const char *fmt, WT_CURSOR_JSON *json,
    int iskey, va_list ap)
{
	WT_CONFIG_ITEM *names;
	WT_DECL_RET;
	size_t needed;
	char **json_bufp;

	if (iskey) {
		names = &json->key_names;
		json_bufp = &json->key_buf;
	} else {
		names = &json->value_names;
		json_bufp = &json->value_buf;
	}
	needed = 0;
	WT_RET(__json_struct_size(session, buffer, size, fmt, names,
	    iskey, &needed));
	WT_RET(__wt_realloc(session, NULL, needed + 1, json_bufp));
	WT_RET(__json_struct_unpackv(session, buffer, size, fmt,
	    names, (u_char *)*json_bufp, needed + 1, iskey, ap));

	return (ret);
}

/*
 * __wt_json_close --
 *	Release any json related resources.
 */
void
__wt_json_close(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
	WT_CURSOR_JSON *json;

	if ((json = (WT_CURSOR_JSON *)cursor->json_private) != NULL) {
		__wt_free(session, json->key_buf);
		__wt_free(session, json->value_buf);
		__wt_free(session, json);
	}
	return;
}

/*
 * __wt_json_unpack_char --
 *	Unpack a single character into JSON escaped format.
 *	Can be called with null buf for sizing.
 */
size_t
__wt_json_unpack_char(char ch, u_char *buf, size_t bufsz, int force_unicode)
{
	char abbrev;
	u_char h;

	if (!force_unicode) {
		if (isprint(ch) && ch != '\\' && ch != '"') {
			if (bufsz >= 1)
				*buf = (u_char)ch;
			return (1);
		} else {
			abbrev = '\0';
			switch (ch) {
			case '\\':
			case '"':
				abbrev = ch;
				break;
			case '\f':
				abbrev = 'f';
				break;
			case '\n':
				abbrev = 'n';
				break;
			case '\r':
				abbrev = 'r';
				break;
			case '\t':
				abbrev = 't';
				break;
			}
			if (abbrev != '\0') {
				if (bufsz >= 2) {
					*buf++ = '\\';
					*buf = (u_char)abbrev;
				}
				return (2);
			}
		}
	}
	if (bufsz >= 6) {
		*buf++ = '\\';
		*buf++ = 'u';
		*buf++ = '0';
		*buf++ = '0';
		h = (((u_char)ch) >> 4) & 0xF;
		if (h >= 10)
			*buf++ = 'A' + (h - 10);
		else
			*buf++ = '0' + h;
		h = ((u_char)ch) & 0xF;
		if (h >= 10)
			*buf++ = 'A' + (h - 10);
		else
			*buf++ = '0' + h;
	}
	return (6);
}

/*
 * __wt_json_column_init --
 *	set json_key_names, json_value_names to comma separated lists
 *	of column names.
 */
int
__wt_json_column_init(WT_CURSOR *cursor, const char *keyformat,
    const WT_CONFIG_ITEM *idxconf, const WT_CONFIG_ITEM *colconf)
{
	WT_CURSOR_JSON *json;
	const char *p, *end, *beginkey;
	uint32_t keycnt, nkeys;

	json = (WT_CURSOR_JSON *)cursor->json_private;
	beginkey = colconf->str;
	end = beginkey + colconf->len;

	if (idxconf != NULL) {
		json->key_names.str = idxconf->str;
		json->key_names.len = idxconf->len;
	} else if (colconf->len > 0 && *beginkey == '(') {
		beginkey++;
		if (end[-1] == ')')
			end--;
	}

	for (nkeys = 0; *keyformat; keyformat++)
		if (!isdigit(*keyformat))
			nkeys++;

	p = beginkey;
	keycnt = 0;
	while (p < end && keycnt < nkeys) {
		if (*p == ',')
			keycnt++;
		p++;
	}
	json->value_names.str = p;
	json->value_names.len = WT_PTRDIFF(end, p);
	if (idxconf == NULL) {
		if (p > beginkey)
			p--;
		json->key_names.str = beginkey;
		json->key_names.len = WT_PTRDIFF(p, beginkey);
	}
	return (0);
}
