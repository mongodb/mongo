/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * 7-bit ASCII, with English language frequencies.
 *
 * Based on "Case-sensitive letter and bigram frequency counts from large-scale
 * English corpora"
 *	Michael N. Jones and D.J.K. Mewhort
 *	Queen's University, Kingston, Ontario, Canada
 * Behavior Research Methods, Instruments, & Computers 2004, 36 (3), 388-396
 *
 * Additionally supports space and tab characters; space is the most common
 * character in text where it occurs, and tab appears about as frequently as
 * 'a' and 'n' in text where it occurs.
 */
static uint8_t const __wt_huffman_ascii_english[256] = {
	1,	/* 000 nul */
	1,	/* 001 soh */
	1,	/* 002 stx */
	1,	/* 003 etx */
	1,	/* 004 eot */
	1,	/* 005 enq */
	1,	/* 006 ack */
	1,	/* 007 bel */
	1,	/* 010 bs  */
	251,	/* 011 ht  */
	1,	/* 012 nl  */
	1,	/* 013 vt  */
	1,	/* 014 np  */
	1,	/* 015 cr  */
	1,	/* 016 so  */
	1,	/* 017 si  */
	1,	/* 020 dle */
	1,	/* 021 dc1 */
	1,	/* 022 dc2 */
	1,	/* 023 dc3 */
	1,	/* 024 dc4 */
	1,	/* 025 nak */
	1,	/* 026 syn */
	1,	/* 027 etb */
	1,	/* 030 can */
	1,	/* 031 em  */
	1,	/* 032 sub */
	1,	/* 033 esc */
	1,	/* 034 fs  */
	1,	/* 035 gs  */
	1,	/* 036 rs  */
	1,	/* 037 us  */
	255,	/* 040 sp  */
	177,	/* 041  !  */
	223,	/* 042  "  */
	171,	/* 043  #  */
	188,	/* 044  $  */
	176,	/* 045  %  */
	179,	/* 046  &  */
	215,	/* 047  '  */
	189,	/* 050  (  */
	190,	/* 051  )  */
	184,	/* 052  *  */
	175,	/* 053  +  */
	234,	/* 054  ,  */
	219,	/* 055  -  */
	233,	/* 056  .  */
	181,	/* 057  /  */
	230,	/* 060  0  */
	229,	/* 061  1  */
	226,	/* 062  2  */
	213,	/* 063  3  */
	214,	/* 064  4  */
	227,	/* 065  5  */
	210,	/* 066  6  */
	203,	/* 067  7  */
	212,	/* 070  8  */
	222,	/* 071  9  */
	191,	/* 072  :  */
	186,	/* 073  ;  */
	173,	/* 074  <  */
	172,	/* 075  =  */
	174,	/* 076  >  */
	183,	/* 077  ?  */
	170,	/* 100  @  */
	221,	/* 101  A  */
	211,	/* 102  B  */
	218,	/* 103  C  */
	206,	/* 104  D  */
	207,	/* 105  E  */
	199,	/* 106  F  */
	197,	/* 107  G  */
	205,	/* 110  H  */
	217,	/* 111  I  */
	196,	/* 112  J  */
	187,	/* 113  K  */
	201,	/* 114  L  */
	220,	/* 115  M  */
	216,	/* 116  N  */
	200,	/* 117  O  */
	208,	/* 120  P  */
	182,	/* 121  Q  */
	209,	/* 122  R  */
	224,	/* 123  S  */
	225,	/* 124  T  */
	193,	/* 125  U  */
	185,	/* 126  V  */
	202,	/* 127  W  */
	180,	/* 130  X  */
	198,	/* 131  Y  */
	178,	/* 132  Z  */
	1,	/* 133  [  */
	1,	/* 134  \  */
	1,	/* 135  ]  */
	1,	/* 136  ^  */
	1,	/* 137  _  */
	1,	/* 140  `  */
	252,	/* 141  a  */
	232,	/* 142  b  */
	242,	/* 143  c  */
	243,	/* 144  d  */
	254,	/* 145  e  */
	239,	/* 146  f  */
	237,	/* 147  g  */
	245,	/* 150  h  */
	248,	/* 151  i  */
	194,	/* 152  j  */
	228,	/* 153  k  */
	244,	/* 154  l  */
	240,	/* 155  m  */
	249,	/* 156  n  */
	250,	/* 157  o  */
	238,	/* 160  p  */
	192,	/* 161  q  */
	246,	/* 162  r  */
	247,	/* 163  s  */
	253,	/* 164  t  */
	241,	/* 165  u  */
	231,	/* 166  v  */
	235,	/* 167  w  */
	204,	/* 170  x  */
	236,	/* 171  y  */
	195,	/* 172  z  */
	1,	/* 173  {  */
	1,	/* 174  |  */
	1,	/* 175  }  */
	1,	/* 176  ~  */
	1,	/* 177 del */
};

/*
 * __wt_db_huffman_set --
 *	DB huffman configuration setter.
 */
int
__wt_db_huffman_set(DB *db,
    uint8_t const *huffman_table, u_int huffman_table_size, uint32_t flags)
{
	ENV *env;
	IDB *idb;
	uint8_t phone[256];

	env = db->env;
	idb = db->idb;

	switch (LF_ISSET(WT_ASCII_ENGLISH | WT_TELEPHONE)) {
	case WT_ASCII_ENGLISH:
		if (huffman_table != NULL)
			goto err;
		huffman_table = __wt_huffman_ascii_english;
		huffman_table_size = sizeof(__wt_huffman_ascii_english);
		break;
	case WT_TELEPHONE:
		if (huffman_table != NULL)
			goto err;
		memset(phone, 0, sizeof(phone));
		phone['('] = 2;
		phone[')'] = 2;
		phone['+'] = 1;
		phone['-'] = 3;
		phone['0'] = 1;
		phone['1'] = 1;
		phone['2'] = 1;
		phone['3'] = 1;
		phone['4'] = 1;
		phone['5'] = 1;
		phone['6'] = 1;
		phone['7'] = 1;
		phone['8'] = 1;
		phone['9'] = 1;
		huffman_table = phone;
		huffman_table_size = sizeof(phone);
		break;
	default:
err:		return (__wt_api_args(env, "Db.huffman_set"));
	}

	/*
	 * If we're using an already-specified table, close it.   It's probably
	 * an application error to set the Huffman table twice, but hey, I just
	 * work here.
	 */
	if (LF_ISSET(WT_HUFFMAN_KEY) && idb->huffman_key != NULL) {
		/* Key and data may use the same table, only close it once. */
		if (idb->huffman_data == idb->huffman_key)
			idb->huffman_data = NULL;
		__wt_huffman_close(env, idb->huffman_key);
		idb->huffman_key = NULL;
	}
	if (LF_ISSET(WT_HUFFMAN_DATA) && idb->huffman_data != NULL) {
		__wt_huffman_close(env, idb->huffman_data);
		idb->huffman_data = NULL;
	}
	if (LF_ISSET(WT_HUFFMAN_KEY)) {
		WT_RET(__wt_huffman_open(env,
		     huffman_table, huffman_table_size, &idb->huffman_key));
		/* Key and data may use the same table. */
		if (LF_ISSET(WT_HUFFMAN_DATA)) {
			idb->huffman_data = idb->huffman_key;
			LF_CLR(WT_HUFFMAN_DATA);
		}
	}
	if (LF_ISSET(WT_HUFFMAN_DATA))
		WT_RET(__wt_huffman_open(env,
		    huffman_table, huffman_table_size, &idb->huffman_data));

	return (0);
}
