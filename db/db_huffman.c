/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * 8-bit ASCII, with English language frequencies.
 *
 * From: "Case-sensitive letter and bigram frequency counts from large-scale
 * English corpora"
 *	Michael N. Jones and D.J.K. Mewhort
 *	Queen's University, Kingston, Ontario, Canada
 * Behavior Research Methods, Instruments, & Computers 2004, 36 (3), 388-396
 */
static u_int8_t const __wt_huffman_english[256] = {
	1,	/* 000 nul */
	1,	/* 001 soh */
	1,	/* 002 stx */
	1,	/* 003 etx */
	1,	/* 004 eot */
	1,	/* 005 enq */
	1,	/* 006 ack */
	1,	/* 007 bel */
	1,	/* 010 bs  */
	1,	/* 011 ht  */
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
	1,	/* 040 sp  */
	179,	/* 041  !  */
	225,	/* 042  "  */
	173,	/* 043  #  */
	190,	/* 044  $  */
	178,	/* 045  %  */
	181,	/* 046  &  */
	217,	/* 047  '  */
	191,	/* 050  (  */
	192,	/* 051  )  */
	186,	/* 052  *  */
	177,	/* 053  +  */
	236,	/* 054  ,  */
	221,	/* 055  -  */
	235,	/* 056  .  */
	183,	/* 057  /  */
	232,	/* 060  0  */
	231,	/* 061  1  */
	228,	/* 062  2  */
	215,	/* 063  3  */
	216,	/* 064  4  */
	229,	/* 065  5  */
	212,	/* 066  6  */
	205,	/* 067  7  */
	214,	/* 070  8  */
	224,	/* 071  9  */
	193,	/* 072  :  */
	188,	/* 073  ;  */
	175,	/* 074  <  */
	174,	/* 075  =  */
	176,	/* 076  >  */
	185,	/* 077  ?  */
	172,	/* 100  @  */
	223,	/* 101  A  */
	213,	/* 102  B  */
	220,	/* 103  C  */
	208,	/* 104  D  */
	209,	/* 105  E  */
	201,	/* 106  F  */
	199,	/* 107  G  */
	207,	/* 110  H  */
	219,	/* 111  I  */
	198,	/* 112  J  */
	189,	/* 113  K  */
	203,	/* 114  L  */
	222,	/* 115  M  */
	218,	/* 116  N  */
	202,	/* 117  O  */
	210,	/* 120  P  */
	184,	/* 121  Q  */
	211,	/* 122  R  */
	226,	/* 123  S  */
	227,	/* 124  T  */
	195,	/* 125  U  */
	187,	/* 126  V  */
	204,	/* 127  W  */
	182,	/* 130  X  */
	200,	/* 131  Y  */
	180,	/* 132  Z  */
	1,	/* 133  [  */
	1,	/* 134  \  */
	1,	/* 135  ]  */
	1,	/* 136  ^  */
	1,	/* 137  _  */
	1,	/* 140  `  */
	253,	/* 141  a  */
	234,	/* 142  b  */
	244,	/* 143  c  */
	245,	/* 144  d  */
	255,	/* 145  e  */
	241,	/* 146  f  */
	239,	/* 147  g  */
	247,	/* 150  h  */
	250,	/* 151  i  */
	196,	/* 152  j  */
	230,	/* 153  k  */
	246,	/* 154  l  */
	242,	/* 155  m  */
	251,	/* 156  n  */
	252,	/* 157  o  */
	240,	/* 160  p  */
	194,	/* 161  q  */
	248,	/* 162  r  */
	249,	/* 163  s  */
	254,	/* 164  t  */
	243,	/* 165  u  */
	233,	/* 166  v  */
	237,	/* 167  w  */
	206,	/* 170  x  */
	238,	/* 171  y  */
	197,	/* 172  z  */
	1,	/* 173  {  */
	1,	/* 174  |  */
	1,	/* 175  }  */
	1,	/* 176  ~  */
	1,	/* 177 del */
};

/*
 * 8-bit ASCII, with phone number frequencies.
 */
static u_int8_t const __wt_huffman_phone[256] = {
	0,	/* 000 nul */
	0,	/* 001 soh */
	0,	/* 002 stx */
	0,	/* 003 etx */
	0,	/* 004 eot */
	0,	/* 005 enq */
	0,	/* 006 ack */
	0,	/* 007 bel */
	0,	/* 010 bs  */
	0,	/* 011 ht  */
	0,	/* 012 nl  */
	0,	/* 013 vt  */
	0,	/* 014 np  */
	0,	/* 015 cr  */
	0,	/* 016 so  */
	0,	/* 017 si  */
	0,	/* 020 dle */
	0,	/* 021 dc1 */
	0,	/* 022 dc2 */
	0,	/* 023 dc3 */
	0,	/* 024 dc4 */
	0,	/* 025 nak */
	0,	/* 026 syn */
	0,	/* 027 etb */
	0,	/* 030 can */
	0,	/* 031 em  */
	0,	/* 032 sub */
	0,	/* 033 esc */
	0,	/* 034 fs  */
	0,	/* 035 gs  */
	0,	/* 036 rs  */
	0,	/* 037 us  */
	0,	/* 040 sp  */
	0,	/* 041  !  */
	0,	/* 042  "  */
	0,	/* 043  #  */
	0,	/* 044  $  */
	0,	/* 045  %  */
	0,	/* 046  &  */
	0,	/* 047  '  */
	2,	/* 050  (  */
	2,	/* 051  )  */
	0,	/* 052  *  */
	1,	/* 053  +  */
	0,	/* 054  ,  */
	2,	/* 055  -  */
	0,	/* 056  .  */
	0,	/* 057  /  */
	1,	/* 060  0  */
	1,	/* 061  1  */
	1,	/* 062  2  */
	1,	/* 063  3  */
	1,	/* 064  4  */
	1,	/* 065  5  */
	1,	/* 066  6  */
	1,	/* 067  7  */
	1,	/* 070  8  */
	1,	/* 071  9  */
	0,	/* 072  :  */
	0,	/* 073  ;  */
	0,	/* 074  <  */
	0,	/* 075  =  */
	0,	/* 076  >  */
	0,	/* 077  ?  */
	0,	/* 100  @  */
	0,	/* 101  A  */
	0,	/* 102  B  */
	0,	/* 103  C  */
	0,	/* 104  D  */
	0,	/* 105  E  */
	0,	/* 106  F  */
	0,	/* 107  G  */
	0,	/* 110  H  */
	0,	/* 111  I  */
	0,	/* 112  J  */
	0,	/* 113  K  */
	0,	/* 114  L  */
	0,	/* 115  M  */
	0,	/* 116  N  */
	0,	/* 117  O  */
	0,	/* 120  P  */
	0,	/* 121  Q  */
	0,	/* 122  R  */
	0,	/* 123  S  */
	0,	/* 124  T  */
	0,	/* 125  U  */
	0,	/* 126  V  */
	0,	/* 127  W  */
	0,	/* 130  X  */
	0,	/* 131  Y  */
	0,	/* 132  Z  */
	0,	/* 133  [  */
	0,	/* 134  \  */
	0,	/* 135  ]  */
	0,	/* 136  ^  */
	0,	/* 137  _  */
	0,	/* 140  `  */
	0,	/* 141  a  */
	0,	/* 142  b  */
	0,	/* 143  c  */
	0,	/* 144  d  */
	0,	/* 145  e  */
	0,	/* 146  f  */
	0,	/* 147  g  */
	0,	/* 150  h  */
	0,	/* 151  i  */
	0,	/* 152  j  */
	0,	/* 153  k  */
	0,	/* 154  l  */
	0,	/* 155  m  */
	0,	/* 156  n  */
	0,	/* 157  o  */
	0,	/* 160  p  */
	0,	/* 161  q  */
	0,	/* 162  r  */
	0,	/* 163  s  */
	0,	/* 164  t  */
	0,	/* 165  u  */
	0,	/* 166  v  */
	0,	/* 167  w  */
	0,	/* 170  x  */
	0,	/* 171  y  */
	0,	/* 172  z  */
	0,	/* 173  {  */
	0,	/* 174  |  */
	0,	/* 175  }  */
	0,	/* 176  ~  */
	0,	/* 177 del */
};

/*
 * __wt_db_huffman_set --
 *	DB huffman configuration setter.
 */
int
__wt_db_huffman_set(DB *db,
    u_int8_t const *huffman_table, int huffman_table_size, u_int32_t flags)
{
	ENV *env;
	IDB *idb;

	env = db->env;
	idb = db->idb;

	switch (LF_ISSET(WT_ENGLISH | WT_PHONE)) {
	case WT_ENGLISH:
		huffman_table = __wt_huffman_english;
		huffman_table_size = sizeof(__wt_huffman_english);
		break;
	case WT_PHONE:
		huffman_table = __wt_huffman_phone;
		huffman_table_size = sizeof(__wt_huffman_phone);
		break;
	default:
		return (__wt_api_flags(env, "Db.huffman_set"));
	}

	/*
	 * If we're using an already-specified table, close it.   It's probably
	 * an application error to set the Huffman table twice, but hey, I just
	 * work here.
	 */
	if (LF_ISSET(WT_HUFFMAN_DATA)) {
		if (idb->huffman_data != NULL) {
			__wt_huffman_close(env, idb->huffman_data);
			idb->huffman_data = NULL;
		}
		WT_RET(__wt_huffman_open(env,
		    huffman_table, huffman_table_size, &idb->huffman_data));
	}

	if (LF_ISSET(WT_HUFFMAN_KEY)) {
		if (idb->huffman_key != NULL) {
			__wt_huffman_close(env, idb->huffman_key);
			idb->huffman_key = NULL;
		}
		WT_RET(__wt_huffman_open(env,
		     huffman_table, huffman_table_size, &idb->huffman_key));
	}

	return (0);
}
