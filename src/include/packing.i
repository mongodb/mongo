/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Throughout this code we have to be aware of default argument conversion.
 *
 * Refer to Chapter 8 of "Expert C Programming" by Peter van der Linden for the
 * gory details.  The short version is that we have less cases to deal with
 * because the compiler promotes shorter types to int or unsigned int.
 */
typedef struct {
	union {
		int64_t i;
		uint64_t u;
		const char *s;
		WT_ITEM item;
	} u;
	uint32_t size;
	int8_t havesize;
	char type;
} WT_PACK_VALUE;

#define	WT_PACK_VALUE_INIT  { { 0 }, 0, 0, 0 }
#define	WT_DECL_PACK_VALUE(pv)  WT_PACK_VALUE pv = WT_PACK_VALUE_INIT

typedef struct {
	WT_SESSION_IMPL *session;
	const char *cur, *end, *orig;
	unsigned long repeats;
	WT_PACK_VALUE lastv;
} WT_PACK;

#define	WT_PACK_INIT    { NULL, NULL, NULL, NULL, 0, WT_PACK_VALUE_INIT }
#define	WT_DECL_PACK(pack)  WT_PACK pack = WT_PACK_INIT

typedef struct {
	WT_CONFIG config;
	char buf[20];
	int count;
	int iskey;
	int genname;
} WT_PACK_NAME;

static inline int
__pack_initn(
    WT_SESSION_IMPL *session, WT_PACK *pack, const char *fmt, size_t len)
{
	if (*fmt == '@' || *fmt == '<' || *fmt == '>')
		return (EINVAL);
	if (*fmt == '.')
		++fmt;

	pack->session = session;
	pack->cur = pack->orig = fmt;
	pack->end = fmt + len;
	pack->repeats = 0;
	return (0);
}

static inline int
__pack_init(WT_SESSION_IMPL *session, WT_PACK *pack, const char *fmt)
{
	return (__pack_initn(session, pack, fmt, strlen(fmt)));
}

static inline int
__pack_name_init(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *names,
    int iskey, WT_PACK_NAME *pn)
{
	WT_CLEAR(*pn);
	pn->iskey = iskey;

	if (names->str != NULL)
		WT_RET(__wt_config_subinit(session, &pn->config, names));
	else
		pn->genname = 1;

	return (0);
}

static inline int
__pack_name_next(WT_PACK_NAME *pn, WT_CONFIG_ITEM *name)
{
	WT_CONFIG_ITEM ignore;

	if (pn->genname) {
		snprintf(pn->buf, sizeof(pn->buf),
		    (pn->iskey ? "key%d" : "value%d"), pn->count);
		WT_CLEAR(*name);
		name->str = pn->buf;
		name->len = strlen(pn->buf);
		name->type = WT_CONFIG_ITEM_STRING;
		pn->count++;
	}
	else
		WT_RET(__wt_config_next(&pn->config, name, &ignore));

	return (0);
}

static inline int
__pack_next(WT_PACK *pack, WT_PACK_VALUE *pv)
{
	char *endsize;

	if (pack->repeats > 0) {
		*pv = pack->lastv;
		--pack->repeats;
		return (0);
	}

next:	if (pack->cur == pack->end)
		return (WT_NOTFOUND);

	if (isdigit(*pack->cur)) {
		pv->havesize = 1;
		pv->size = WT_STORE_SIZE(strtoul(pack->cur, &endsize, 10));
		pack->cur = endsize;
	} else {
		pv->havesize = 0;
		pv->size = 1;
	}

	pv->type = *pack->cur++;
	pack->repeats = 0;

	switch (pv->type) {
	case 'S':
	case 's':
	case 'x':
		return (0);
	case 't':
		if (pv->size < 1 || pv->size > 8)
			WT_RET_MSG(pack->session, EINVAL,
			    "Bitfield sizes must be between 1 and 8 bits "
			    "in format '%.*s'",
			    (int)(pack->end - pack->orig), pack->orig);
		return (0);
	case 'u':
	case 'U':
		/* Special case for items with a size prefix. */
		pv->type = (!pv->havesize && *pack->cur != '\0') ? 'U' : 'u';
		return (0);
	case 'b':
	case 'h':
	case 'i':
	case 'B':
	case 'H':
	case 'I':
	case 'l':
	case 'L':
	case 'q':
	case 'Q':
	case 'r':
	case 'R':
		/* Integral types repeat <size> times. */
		if (pv->size == 0)
			goto next;
		pack->repeats = pv->size - 1;
		pack->lastv = *pv;
		return (0);
	default:
		WT_RET_MSG(pack->session, EINVAL,
		   "Invalid type '%c' found in format '%.*s'",
		    pv->type, (int)(pack->end - pack->orig), pack->orig);
	}

}

#define	WT_PACK_GET(session, pv, ap) do {				\
	WT_ITEM *__item;						\
	switch (pv.type) {						\
	case 'x':							\
		break;							\
	case 's':							\
	case 'S':							\
		pv.u.s = va_arg(ap, const char *);			\
		break;							\
	case 'U':							\
	case 'u':							\
		__item = va_arg(ap, WT_ITEM *);				\
		pv.u.item.data = __item->data;				\
		pv.u.item.size = __item->size;				\
		break;							\
	case 'b':							\
	case 'h':							\
	case 'i':							\
		pv.u.i = va_arg(ap, int);				\
		break;							\
	case 'B':							\
	case 'H':							\
	case 'I':							\
	case 't':							\
		pv.u.u = va_arg(ap, unsigned int);			\
		break;							\
	case 'l':							\
		pv.u.i = va_arg(ap, long);				\
		break;							\
	case 'L':							\
		pv.u.u = va_arg(ap, unsigned long);			\
		break;							\
	case 'q':							\
		pv.u.i = va_arg(ap, int64_t);				\
		break;							\
	case 'Q':							\
	case 'r':							\
	case 'R':							\
		pv.u.u = va_arg(ap, uint64_t);				\
		break;							\
	/* User format strings have already been validated. */          \
	WT_ILLEGAL_VALUE(session);                                      \
	}								\
} while (0)

static inline size_t
__pack_size(WT_SESSION_IMPL *session, WT_PACK_VALUE *pv)
{
	size_t s, pad;

	switch (pv->type) {
	case 'x':
		return (pv->size);
	case 's':
	case 'S':
		if (pv->type == 's' || pv->havesize)
			s = pv->size;
		else
			s = strlen(pv->u.s) + 1;
		return (s);
	case 'U':
	case 'u':
		s = pv->u.item.size;
		pad = 0;
		if (pv->havesize && pv->size < s)
			s = pv->size;
		else if (pv->havesize)
			pad = pv->size - s;
		if (pv->type == 'U')
			s += __wt_vsize_uint(s + pad);
		return (s + pad);
	case 'b':
	case 'B':
	case 't':
		return (1);
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		return (__wt_vsize_int(pv->u.i));
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'r':
		return (__wt_vsize_uint(pv->u.u));
	case 'R':
		return (sizeof (uint64_t));
	}

	__wt_err(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
	return ((size_t)-1);
}

/*
 * __unpack_json_char --
 *	Unpack a single character into JSON escaped format.
 *	Can be called with null buf for sizing.
 */
static inline size_t __unpack_json_char(char ch, char *buf, size_t bufsz,
    int force_unicode)
{
	char abbrev;
	u_char h;

	if (!force_unicode) {
		if (isprint(ch) && ch != '\\' && ch != '"') {
			if (bufsz >= 1) {
				*buf = ch;
			}
			return (1);
		}
		else {
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
					*buf = abbrev;
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

static inline size_t
__unpack_put_json(WT_SESSION_IMPL *session, WT_PACK_VALUE *pv,
    char *buf, size_t bufsz, WT_CONFIG_ITEM *name)
{
	const char *p, *end;
	size_t s, n;

	s = snprintf(buf, bufsz, "\"%.*s\" : ", (int)name->len, name->str);
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
			for ( ; p < end; p++) {
				n = __unpack_json_char(*p, buf, bufsz, 0);
				if (n > bufsz)
					bufsz = 0;
				else {
					bufsz -= n;
					buf += n;
				}
				s += n;
			}
		} else
			for ( ; *p; p++) {
				n = __unpack_json_char(*p, buf, bufsz, 0);
				if (n > bufsz)
					bufsz = 0;
				else {
					bufsz -= n;
					buf += n;
				}
				s += n;
			}
		if (bufsz > 0) {
			*buf++ = '"';
			bufsz--;
		}
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
		for ( ; p < end; p++) {
			n = __unpack_json_char(*p, buf, bufsz, 1);
			if (n > bufsz)
				bufsz = 0;
			else {
				bufsz -= n;
				buf += n;
			}
			s += n;
		}
		if (bufsz > 0) {
			*buf++ = '"';
			bufsz--;
		}
		return (s);
	case 'b':
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		return (s + snprintf(buf, bufsz, "%" PRId64, pv->u.i));
	case 'B':
	case 't':
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'r':
	case 'R':
		return (s + snprintf(buf, bufsz, "%" PRId64, pv->u.u));
	}
	__wt_err(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
	return ((size_t)-1);
}

static inline int
__pack_write(
    WT_SESSION_IMPL *session, WT_PACK_VALUE *pv, uint8_t **pp, size_t maxlen)
{
	uint8_t *oldp;
	size_t s, pad;

	switch (pv->type) {
	case 'x':
		WT_SIZE_CHECK(pv->size, maxlen);
		memset(*pp, 0, pv->size);
		*pp += pv->size;
		break;
	case 's':
	case 'S':
		/*
		 * XXX if pv->havesize, only want to know if there is a
		 * '\0' in the first pv->size characters.
		 */
		s = strlen(pv->u.s);
		if ((pv->type == 's' || pv->havesize) && pv->size < s) {
			s = pv->size;
			pad = 0;
		} else if (pv->havesize)
			pad = pv->size - s;
		else
			pad = 1;
		WT_SIZE_CHECK(s + pad, maxlen);
		if (s > 0)
			memcpy(*pp, pv->u.s, s);
		*pp += s;
		if (pad > 0) {
			memset(*pp, 0, pad);
			*pp += pad;
		}
		break;
	case 'U':
	case 'u':
		s = pv->u.item.size;
		pad = 0;
		if (pv->havesize && pv->size < s)
			s = pv->size;
		else if (pv->havesize)
			pad = pv->size - s;
		if (pv->type == 'U') {
			oldp = *pp;
			WT_RET(__wt_vpack_uint(pp, maxlen, s + pad));
			maxlen -= (size_t)(*pp - oldp);
		}
		WT_SIZE_CHECK(s + pad, maxlen);
		if (s > 0)
			memcpy(*pp, pv->u.item.data, s);
		*pp += s;
		if (pad > 0) {
			memset(*pp, 0, pad);
			*pp += pad;
		}
		break;
	case 'b':
		/* Translate to maintain ordering with the sign bit. */
		WT_SIZE_CHECK(1, maxlen);
		**pp = (uint8_t)(pv->u.i + 0x80);
		*pp += 1;
		break;
	case 'B':
	case 't':
		WT_SIZE_CHECK(1, maxlen);
		**pp = (uint8_t)pv->u.u;
		*pp += 1;
		break;
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		WT_RET(__wt_vpack_int(pp, maxlen, pv->u.i));
		break;
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'r':
		WT_RET(__wt_vpack_uint(pp, maxlen, pv->u.u));
		break;
	case 'R':
		WT_SIZE_CHECK(sizeof (uint64_t), maxlen);
		*(uint64_t *)*pp = pv->u.u;
		*pp += sizeof(uint64_t);
		break;
	default:
		WT_RET_MSG(session, EINVAL,
		    "unknown pack-value type: %c", (int)pv->type);
	}

	return (0);
}

static inline int
__unpack_read(WT_SESSION_IMPL *session,
    WT_PACK_VALUE *pv, const uint8_t **pp, size_t maxlen)
{
	size_t s;

	switch (pv->type) {
	case 'x':
		WT_SIZE_CHECK(pv->size, maxlen);
		*pp += pv->size;
		break;
	case 's':
	case 'S':
		if (pv->type == 's' || pv->havesize)
			s = pv->size;
		else
			s = strlen((const char *)*pp) + 1;
		if (s > 0)
			pv->u.s = (const char *)*pp;
		WT_SIZE_CHECK(s, maxlen);
		*pp += s;
		break;
	case 'U':
		WT_RET(__wt_vunpack_uint(pp, maxlen, &pv->u.u));
		/* FALLTHROUGH */
	case 'u':
		if (pv->havesize)
			s = pv->size;
		else if (pv->type == 'U')
			s = (size_t)pv->u.u;
		else
			s = maxlen;
		WT_SIZE_CHECK(s, maxlen);
		pv->u.item.data = *pp;
		pv->u.item.size = s;
		*pp += s;
		break;
	case 'b':
		/* Translate to maintain ordering with the sign bit. */
		WT_SIZE_CHECK(1, maxlen);
		pv->u.i = (int8_t)(*(*pp)++ - 0x80);
		break;
	case 'B':
	case 't':
		WT_SIZE_CHECK(1, maxlen);
		pv->u.u = *(*pp)++;
		break;
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		WT_RET(__wt_vunpack_int(pp, maxlen, &pv->u.i));
		break;
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'r':
		WT_RET(__wt_vunpack_uint(pp, maxlen, &pv->u.u));
		break;
	case 'R':
		WT_SIZE_CHECK(sizeof (uint64_t), maxlen);
		pv->u.u = *(uint64_t *)*pp;
		*pp += sizeof(uint64_t);
		break;
	default:
		WT_RET_MSG(session, EINVAL,
		    "unknown pack-value type: %c", (int)pv->type);
	}

	return (0);
}

#define	WT_UNPACK_PUT(session, pv, ap) do {				\
	WT_ITEM *__item;						\
	switch (pv.type) {						\
	case 'x':							\
		break;							\
	case 's':							\
	case 'S':							\
		*va_arg(ap, const char **) = pv.u.s;			\
		break;							\
	case 'U':							\
	case 'u':							\
		__item = va_arg(ap, WT_ITEM *);				\
		__item->data = pv.u.item.data;				\
		__item->size = pv.u.item.size;				\
		break;							\
	case 'b':							\
		*va_arg(ap, int8_t *) = (int8_t)pv.u.i;			\
		break;							\
	case 'h':							\
		*va_arg(ap, int16_t *) = (short)pv.u.i;			\
		break;							\
	case 'i':							\
	case 'l':							\
		*va_arg(ap, int32_t *) = (int32_t)pv.u.i;		\
		break;							\
	case 'q':							\
		*va_arg(ap, int64_t *) = pv.u.i;			\
		break;							\
	case 'B':							\
	case 't':							\
		*va_arg(ap, uint8_t *) = (uint8_t)pv.u.u;		\
		break;							\
	case 'H':							\
		*va_arg(ap, uint16_t *) = (uint16_t)pv.u.u;             \
		break;							\
	case 'I':							\
	case 'L':							\
		*va_arg(ap, uint32_t *) = (uint32_t)pv.u.u;	        \
		break;							\
	case 'Q':							\
	case 'r':							\
	case 'R':							\
		*va_arg(ap, uint64_t *) = pv.u.u;			\
		break;							\
	/* User format strings have already been validated. */          \
	WT_ILLEGAL_VALUE(session);                                      \
	}								\
} while (0)

/*
 * __wt_struct_json_size --
 *	Calculate the size of a packed byte string as formatted for JSON.
 */
static inline int
__wt_struct_json_size(WT_SESSION_IMPL *session, const void *buffer,
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

	WT_ERR(__pack_name_init(session, names, iskey, &packname));
	WT_ERR(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0) {
		if (needcr)
			result += 2;
		needcr = 1;
		WT_ERR(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_ERR(__pack_name_next(&packname, &name));
		result += __unpack_put_json(session, &pv, NULL, 0, &name);
	}
	if (ret == WT_NOTFOUND)
		ret = 0;

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	*presult = result;
err:	return (0);
}

/*
 * __wt_struct_json_unpackv --
 *	Unpack a byte string to JSON (va_list version).
 */
static inline int
__wt_struct_json_unpackv(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, WT_CONFIG_ITEM *names,
    char *jbuf, size_t jbufsize, int iskey, va_list ap)
{
	WT_CONFIG_ITEM name;
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_NAME packname;
	int needcr;
	size_t jsize;
	const uint8_t *p, *end;

	p = buffer;
	end = p + size;
	needcr = 0;

	/* Unpacking a cursor marked as json implies a single WT_ITEM arg. */
	item = va_arg(ap, WT_ITEM *);
	item->data = jbuf;
	item->size = jbufsize - 1;  /* don't include null */

	WT_ERR(__pack_name_init(session, names, iskey, &packname));
	WT_ERR(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0) {
		if (needcr) {
			WT_ASSERT(session, jbufsize >= 3);
			strncat(jbuf, ",\n", jbufsize);
			jbuf += 2;
			jbufsize -= 2;
		}
		needcr = 1;
		WT_ERR(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_ERR(__pack_name_next(&packname, &name));
		jsize = __unpack_put_json(session, &pv, jbuf, jbufsize, &name);
		WT_ASSERT(session, jsize <= jbufsize);
		jbuf += jsize;
		jbufsize -= jsize;
	}
	if (ret == WT_NOTFOUND)
		ret = 0;

	/* Be paranoid - __unpack_read should never overflow. */
	WT_ASSERT(session, p <= end);

	WT_ASSERT(session, jbufsize == 1);

err:	return (ret);
}

/*
 * __wt_struct_packv --
 *	Pack a byte string (va_list version).
 */
static inline int
__wt_struct_packv(WT_SESSION_IMPL *session,
    void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_PACK pack;
	uint8_t *p, *end;

	p = buffer;
	end = p + size;

	if (fmt[0] != '\0' && fmt[1] == '\0') {
		pv.type = fmt[0];
		WT_PACK_GET(session, pv, ap);
		return (__pack_write(session, &pv, &p, size));
	}

	WT_RET(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0) {
		WT_PACK_GET(session, pv, ap);
		WT_RET(__pack_write(session, &pv, &p, (size_t)(end - p)));
	}

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}

/*
 * __wt_struct_sizev --
 *	Calculate the size of a packed byte string (va_list version).
 */
static inline int
__wt_struct_sizev(
    WT_SESSION_IMPL *session, size_t *sizep, const char *fmt, va_list ap)
{
	WT_DECL_PACK_VALUE(pv);
	WT_PACK pack;
	size_t total;

	if (fmt[0] != '\0' && fmt[1] == '\0') {
		pv.type = fmt[0];
		WT_PACK_GET(session, pv, ap);
		*sizep = __pack_size(session, &pv);
		return (0);
	}

	WT_RET(__pack_init(session, &pack, fmt));
	for (total = 0; __pack_next(&pack, &pv) == 0;) {
		WT_PACK_GET(session, pv, ap);
		total += __pack_size(session, &pv);
	}
	*sizep = total;
	return (0);
}

/*
 * __wt_struct_unpackv --
 *	Unpack a byte string (va_list version).
 */
static inline int
__wt_struct_unpackv(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_PACK pack;
	const uint8_t *p, *end;

	p = buffer;
	end = p + size;

	if (fmt[0] != '\0' && fmt[1] == '\0') {
		pv.type = fmt[0];
		if ((ret = __unpack_read(session, &pv, &p, size)) == 0)
			WT_UNPACK_PUT(session, pv, ap);
		return (0);
	}

	WT_RET(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0) {
		WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_UNPACK_PUT(session, pv, ap);
	}

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}
