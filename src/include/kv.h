/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_KV_RECNO(key_format) (strcmp((key_format), "r") == 0)

/*
 * get_key worker macro that can handle either a cursor or async op handle.
 *	Assumes the following variables are available:
 *	ap, fmt, key, ret, session, size.
 */
#define	WT_KV_GET_KEY(handle, f, rawflag, rawok) do {			\
	if (WT_KV_RECNO((handle)->key_format)) {			\
		if (FLD_ISSET((f), rawflag)) {				\
			key = va_arg(ap, WT_ITEM *);			\
			key->data = (handle)->raw_recno_buf;		\
			WT_ERR(__wt_struct_size(			\
			    session, &size, "q", (handle)->recno));	\
			key->size = size;				\
			ret = __wt_struct_pack(session,			\
			    (handle)->raw_recno_buf,			\
			    sizeof((handle)->raw_recno_buf),		\
			    "q", (handle)->recno);			\
		} else							\
			*va_arg(ap, uint64_t *) = (handle)->recno;	\
	} else {							\
		fmt =							\
		    FLD_ISSET((f), rawok) ? "u" : (handle)->key_format;	\
		/* Fast path some common cases. */			\
		if (strcmp(fmt, "S") == 0)				\
			*va_arg(ap, const char **) = (handle)->key.data;\
		else if (strcmp(fmt, "u") == 0) {			\
			key = va_arg(ap, WT_ITEM *);			\
			key->data = (handle)->key.data;			\
			key->size = (handle)->key.size;			\
		} else							\
			ret = __wt_struct_unpackv(session,		\
			    (handle)->key.data, (handle)->key.size,	\
			    fmt, ap);					\
	}								\
} while (0)

/* Perform a key or value raw operation */
#define	WT_WITH_RAW(handle, rawflag, e) do {				\
	int __raw_set;							\
	__raw_set = F_ISSET((handle), (rawflag)) ? 1 : 0;		\
	if (!__raw_set)							\
		F_SET((handle), (rawflag));				\
	e;								\
	if (!__raw_set)							\
		F_CLR((handle), (rawflag));				\
} while (0)
