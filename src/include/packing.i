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
	union {
		int64_t i;
		uint64_t u;
		const char *s;
		WT_ITEM *item;
	} u;
	int havesize;
	size_t size;
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

	pv->size = strtoul(pack->cur, &endsize, 10);
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
