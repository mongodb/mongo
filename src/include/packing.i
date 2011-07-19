/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * Throughout this code we have to be aware of default argument conversion.
 *
 * Refer to Chapter 8 of "Expert C Programming" by Peter van der Linden for the
 * gory details.  The short version is that we have less cases to deal with
 * because the compiler promotes shorter types to int or unsigned int.
 */

typedef struct {
	WT_SESSION_IMPL *session;
	const char *cur, *end, *orig;
	unsigned long repeats;
} WT_PACK;

typedef struct {
	char type;
	int8_t havesize;
	uint32_t size;
	union {
		int64_t i;
		uint64_t u;
		const char *s;
		WT_ITEM item;
	} u;
} WT_PACK_VALUE;

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
__pack_next(WT_PACK *pack, WT_PACK_VALUE *pv)
{
	char *endsize;

	if (pack->repeats > 0) {
		--pack->repeats;
		return (0);
	}

next:	if (pack->cur == pack->end)
		return (WT_NOTFOUND);

	pv->size = (uint32_t)strtoul(pack->cur, &endsize, 10);
	pv->havesize = (endsize > pack->cur);
	if (!pv->havesize)
		pv->size = 1;
	pack->cur = endsize;
	pack->repeats = 0;
	pv->type = *pack->cur++;

	switch (pv->type) {
	case 'S':
	case 's':
	case 'x':
		return (0);
	case 't':
		if (pv->size < 1 || pv->size > 8) {
			__wt_errx(pack->session,
			    "Bitfield sizes must be between 1 and 8 bits "
			    "in format '%.*s'",
			    (int)(pack->end - pack->orig), pack->orig);
			return (EINVAL);
		}
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
		/* Integral types repeat <size> times. */
		if (pv->size == 0)
			goto next;
		pack->repeats = pv->size - 1;
		return (0);
	default:
		__wt_errx(pack->session,
		   "Invalid type '%c' found in format '%.*s'",
		    pv->type, (int)(pack->end - pack->orig), pack->orig);
		return (EINVAL);
	}

}

#define	WT_PACK_GET(session, pv, ap) do {				\
	switch (pv.type) {						\
	case 's':							\
	case 'S':							\
		pv.u.s = va_arg(ap, const char *);			\
		break;							\
	case 'U':							\
	case 'u':							\
		pv.u.item = *va_arg(ap, WT_ITEM *);			\
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
		pv.u.u = va_arg(ap, uint64_t);				\
		break;							\
	default:							\
		WT_ASSERT(session, pv.type != pv.type);			\
		break;							\
	}								\
} while (0)

static inline size_t
__pack_size(WT_SESSION_IMPL *session, WT_PACK_VALUE *pv)
{
	const char *p, *start, *end;
	size_t s, pad;

	switch (pv->type) {
	case 'x':
		return (pv->size);
	case 's':
	case 'S':
		/*
		 * XXX if pv->havesize, only want to know if there is a
		 * '\0' in the first pv->size characters.
		 */
		if (pv->type == 's' || pv->havesize) {
			p = start = (const char *)pv->u.s;
			end = start + pv->size;
			while (p < end && *p != '\0')
				p++;
			s = (size_t)(p - start);
			pad = pv->size - s;
		} else {
			s = strlen(pv->u.s);
			pad = 1;
		}
		return (s + pad);
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
	}

	WT_FAILURE(session, "unknown pack-value type: %c", (int)pv->type);
	return (0);
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
	default:
		WT_FAILURE(
		    session, "unknown pack-value type: %c", (int)pv->type);
		break;
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
		s = (size_t)pv->u.u;
		/* FALLTHROUGH */
	case 'u':
		if (pv->havesize)
			s = pv->size;
		else if (pv->type != 'U')
			s = maxlen;
		WT_SIZE_CHECK(s, maxlen);
		pv->u.item.data = *pp;
		pv->u.item.size = (uint32_t)s;
		*pp += s;
		break;
	case 'b':
		/* Translate to maintain ordering with the sign bit. */
		WT_SIZE_CHECK(1, maxlen);
		pv->u.i = (int8_t)(**pp++ - 0x80);
		break;
	case 'B':
	case 't':
		WT_SIZE_CHECK(1, maxlen);
		pv->u.u = **pp++;
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
	default:
		WT_FAILURE(
		    session, "unknown pack-value type: %c", (int)pv->type);
		break;
	}

	return (0);
}

#define	WT_UNPACK_PUT(session, pv, ap) do {				\
	switch (pv.type) {						\
	case 's':							\
	case 'S':							\
		*va_arg(ap, const char **) = pv.u.s;			\
		break;							\
	case 'U':							\
	case 'u':							\
		*va_arg(ap, WT_ITEM *) = pv.u.item;			\
		break;							\
	case 'b':							\
		*va_arg(ap, int8_t *) = (int8_t)pv.u.i;			\
		break;							\
	case 'h':							\
		*va_arg(ap, short *) = (short)pv.u.i;			\
		break;							\
	case 'i':							\
		*va_arg(ap, int *) = (int)pv.u.i;			\
		break;							\
	case 'l':							\
		*va_arg(ap, long *) = (long)pv.u.i;			\
		break;							\
	case 'q':							\
		*va_arg(ap, int64_t *) = pv.u.i;			\
		break;							\
	case 'B':							\
	case 't':							\
		*va_arg(ap, uint8_t *) = (uint8_t)pv.u.u;		\
		break;							\
	case 'H':							\
		*va_arg(ap, unsigned short *) = (unsigned short)pv.u.u; \
		break;							\
	case 'I':							\
		*va_arg(ap, unsigned int *) = (unsigned int)pv.u.u;	\
		break;							\
	case 'L':							\
		*va_arg(ap, unsigned long *) = (unsigned long)pv.u.u;	\
		break;							\
	case 'Q':							\
	case 'r':							\
		*va_arg(ap, uint64_t *) = pv.u.u;			\
		break;							\
	default:							\
		WT_ASSERT(session, pv.type != pv.type);			\
		break;							\
	}								\
} while (0)
