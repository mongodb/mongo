/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef	__WT_HASH_H
#define	__WT_HASH_H

/*
 * Header file to for hash implementations used by Wired Tiger.
 */

/*
 * Google City Hash C port. Based on source code from:
 * http://code.google.com/p/cityhash-c/
 */

typedef struct _uint128 uint128;
struct _uint128 {
  uint64_t first;
  uint64_t second;
};

#define	Uint128Low64(x) 	(x).first
#define	Uint128High64(x)	(x).second

/*
 * FNV 1a Hash. Based on source code from:
 * http://www.isthe.com/chongo/src/fnv/fnv.h
 */

/*
 * 64 bit FNV-0 hash
 */
typedef uint64_t Fnv64_t;

/*
 * 64 bit FNV-1 non-zero initial basis
 *
 * The FNV-1 initial basis is the FNV-0 hash of the following 32 octets:
 *
 *              chongo <Landon Curt Noll> /\../\
 *
 * NOTE: The \'s above are not back-slashing escape characters.
 * They are literal ASCII  backslash 0x5c characters.
 *
 * NOTE: The FNV-1a initial basis is the same value as FNV-1 by definition.
 */
#define	FNV1A_64_INIT ((Fnv64_t)0xcbf29ce484222325ULL)

#endif /* __WT_HASH_H */
