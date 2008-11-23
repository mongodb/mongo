/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Database file offsets are multiples of 512B and stored internally in 32-bit
 * variables.
 */
#define	WT_BLOCK_SIZE			(512)
#define	WT_BLOCKS_TO_BYTES(blocks)	(blocks) * WT_BLOCK_SIZE

/*
 * Standard flag checking at every API function.
 */
#define	API_FLAG_CHK(ienv, name, f, mask)				\
	if ((f) & ~(mask))						\
		return (__wt_api_flags(ienv, name));

#define	API_FLAG_CHK_NOTFATAL(ienv, name, f, mask)			\
	if ((f) & ~(mask))						\
		(void)__wt_api_flags(ienv, name);

/*
 * Flag set, clear and test.  They come in 2 flavors: F_XXX (manipulates
 * a field named "flags" in the structure referenced by its argument),
 * and LF_XXX (manipulates a local variable named "flags").
 */
#define	F_CLR(p, mask)		(p)->flags &= ~(mask)
#define	F_ISSET(p, mask)	((p)->flags & (mask))
#define	F_SET(p, mask)		(p)->flags |= (mask)

#define	LF_CLR(mask)		((flags) &= ~(mask))
#define	LF_ISSET(mask)		((flags) & (mask))
#define	LF_SET(mask)		((flags) |= (mask))

/*
 * Statistics are optional to minimize our footprint.
 */
#ifdef	HAVE_STATISTICS
#define	WT_STAT_DECL(v)	wt_stat_t v
#define	WT_STAT(v)	v

/* By default, statistics are maintained in 64-bit types to avoid overflow. */
typedef	u_int64_t	wt_stat_t;

#else
#define	WT_STAT_DECL(v)
#define	WT_STAT(v)
#endif

/* A distinguished byte pattern to overwrite memory we are done using. */
#define	OVERWRITE_BYTE	0xab

#ifdef HAVE_DEBUG
#define WT_ASSERT(ienv, e)						\
	((e) ? (void)0 : __wt_assert(ienv, #e, __FILE__, __LINE__))
#else
#define WT_ASSERT(ienv, e)
#endif

#if defined(__cplusplus)
}
#endif
