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

static int
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

static int
__pack_init(WT_SESSION_IMPL *session, WT_PACK *pack, const char *fmt)
{
	return (__pack_initn(session, pack, fmt, strlen(fmt)));
}

static int
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
	pv->type = *pack->cur++;

	switch (pv->type) {
	case 'u':
		/* Special case for items with a size prefix. */
		if (!pv->havesize && *pack->cur != '\0')
			pv->type = 'U';
		/* FALLTHROUGH */
	case 'x':
	case 's':
	case 'S':
		pack->repeats = 0;
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
		WT_ASSERT(NULL, pv->type != pv->type);
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

#define	WT_PACK_SIZE(session, pv, total) do {				\
	size_t len, padding;						\
									\
	switch (pv.type) {						\
	case 'x':							\
		total += pv.size;					\
		break;							\
	case 's':							\
	case 'S':							\
		/*							\
		 * XXX if pv.havesize, only want to know if there is a	\
		 * '\0' in the first pv.size characters.		\
		 */							\
		len = strlen(pv.u.s);					\
		if ((pv.type == 's' || pv.havesize) && pv.size < len) { \
			len = pv.size;					\
			padding = 0;					\
		} else if (pv.havesize)					\
			padding = pv.size - len;			\
		else							\
			padding = 1;					\
		total += len + padding;					\
		break;							\
	case 'U':							\
	case 'u':							\
		len = pv.u.item.size;					\
		padding = 0;						\
		if (pv.havesize && pv.size < len)			\
			len = pv.size;					\
		else if (pv.havesize)					\
			padding = pv.size - len;			\
		if (pv.type == 'U')					\
			total += __wt_vsize_uint(len + padding);	\
		total += len + padding;					\
		break;							\
	case 'b':							\
	case 'h':							\
	case 'i':							\
	case 'l':							\
	case 'q':							\
		total += __wt_vsize_int(pv.u.i);			\
		break;							\
	case 'B':							\
	case 'H':							\
	case 'I':							\
	case 'L':							\
	case 'Q':							\
	case 'r':							\
		total += __wt_vsize_uint(pv.u.u);			\
		break;							\
	}								\
} while (0)

#define	WT_PACK_WRITE(session, pv, p, end) do {				\
	size_t len, padding;						\
									\
	switch (pv.type) {						\
	case 'x':							\
		memset(p, 0, pv.size);					\
		p += pv.size;						\
		break;							\
	case 's':							\
	case 'S':							\
		/*							\
		 * XXX if pv.havesize, only want to know if there is a	\
		 * '\0' in the first pv.size characters.		\
		 */							\
		len = strlen(pv.u.s);					\
		if ((pv.type == 's' || pv.havesize) && pv.size < len) { \
			len = pv.size;					\
			padding = 0;					\
		} else if (pv.havesize)					\
			padding = pv.size - len;			\
		else							\
			padding = 1;					\
		if (p + len + padding > end)				\
			return (ENOMEM);				\
		if (len > 0)						\
			memcpy(p, pv.u.s, len);				\
		p += len;						\
		if (padding > 0)					\
			memset(p, 0, padding);				\
		p += padding;						\
		break;							\
	case 'U':							\
	case 'u':							\
		len = pv.u.item.size;					\
		padding = 0;						\
		if (pv.havesize && pv.size < len)			\
			len = pv.size;					\
		else if (pv.havesize)					\
			padding = pv.size - len;			\
		if (pv.type == 'U')					\
			WT_RET(__wt_vpack_uint(session,			\
			     &p, (size_t)(end - p), len + padding));	\
		if (p + len + padding > end)				\
			return (ENOMEM);				\
		if (len > 0)						\
			memcpy(p, pv.u.item.data, len);			\
		p += len;						\
		if (padding > 0)					\
			memset(p, 0, padding);				\
		p += padding;						\
		break;							\
	case 'b':							\
	case 'h':							\
	case 'i':							\
	case 'l':							\
	case 'q':							\
		WT_RET(__wt_vpack_int(session,				\
		     &p, (size_t)(end - p), pv.u.i));			\
		break;							\
	case 'B':							\
	case 'H':							\
	case 'I':							\
	case 'L':							\
	case 'Q':							\
	case 'r':							\
		WT_RET(__wt_vpack_uint(session,				\
		     &p, (size_t)(end - p), pv.u.u));			\
		break;							\
	default:							\
		WT_ASSERT(session, pv.type != pv.type);			\
		break;							\
	}								\
} while (0)

#define	WT_UNPACK_READ(session, pv, p, end) do {			\
	size_t len;						\
									\
	switch (pv.type) {						\
	case 'x':							\
		p += pv.size;						\
		break;							\
	case 's':							\
	case 'S':							\
		if (pv.type == 's' || pv.havesize)			\
			len = pv.size;					\
		else							\
			len = strlen((const char *)p) + 1;		\
		if (len > 0)						\
			pv.u.s = (const char *)p;			\
		p += len;						\
		break;							\
	case 'U':							\
		WT_RET(__wt_vunpack_uint(session,			\
		     &p, (size_t)(end - p), &pv.u.u));			\
		len = (size_t)pv.u.u;					\
		/* FALLTHROUGH */					\
	case 'u':							\
		if (pv.havesize)					\
			len = pv.size;					\
		else if (pv.type != 'U')				\
			len = (size_t)(end - p);			\
		pv.u.item.data = p;					\
		pv.u.item.size = (uint32_t)len;				\
		p += len;						\
		break;							\
	case 'b':							\
	case 'h':							\
	case 'i':							\
	case 'l':							\
	case 'q':							\
		WT_RET(__wt_vunpack_int(session,			\
		     &p, (size_t)(end - p), &pv.u.i));			\
		break;							\
	case 'B':							\
	case 'H':							\
	case 'I':							\
	case 'L':							\
	case 'Q':							\
	case 'r':							\
		WT_RET(__wt_vunpack_uint(session,			\
		     &p, (size_t)(end - p), &pv.u.u));			\
		break;							\
	default:							\
		WT_ASSERT(session, pv.type != pv.type);			\
		break;							\
	}								\
} while (0)

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
