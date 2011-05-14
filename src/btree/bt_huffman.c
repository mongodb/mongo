/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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

static int __wt_huffman_read(SESSION *, WT_CONFIG_ITEM *, uint8_t **, u_int *);

/*
 * __wt_btree_huffman_open --
 *	Configure Huffman encoding for the tree.
 */
int
__wt_btree_huffman_open(SESSION *session)
{
	BTREE *btree;
	u_int huffman_table_entries;
	uint8_t *huffman_table;
	WT_CONFIG_ITEM key_conf, value_conf;
	const char *config;
	int ret;

	btree = session->btree;
	config = btree->config;

	WT_RET(__wt_config_getones(config, "huffman_key", &key_conf));
	WT_RET(__wt_config_getones(config, "huffman_value", &value_conf));
	if (key_conf.len == 0 && value_conf.len == 0)
		return (0);

	switch (btree->type) {		/* Check file type compatibility. */
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
		__wt_errx(session,
		    "fixed-size column-store files may not be Huffman encoded");
		return (WT_ERROR);
	case BTREE_COL_VAR:
		if (key_conf.len != 0) {
			__wt_errx(session,
			    "the keys of variable-length column-store files "
			    "may not be Huffman encoded");
			return (WT_ERROR);
		}
		break;
	case BTREE_ROW:
		break;
	}

	if (strncasecmp(key_conf.str, "english", key_conf.len) == 0) {
		WT_RET(__wt_huffman_open(session, __wt_huffman_ascii_english,
		    sizeof(__wt_huffman_ascii_english), &btree->huffman_key));

		/* Check for a shared key/value table. */
		if (strncasecmp(
		    value_conf.str, "english", value_conf.len) == 0) {
			btree->huffman_value = btree->huffman_key;
			return (0);
		}
	} else {
		WT_RET(__wt_huffman_read(session,
		    &key_conf, &huffman_table, &huffman_table_entries));
		ret = __wt_huffman_open(session,
		    huffman_table, huffman_table_entries, &btree->huffman_key);
		__wt_free(session, huffman_table);
		if (ret != 0)
			return (ret);

		/* Check for a shared key/value table. */
		if (value_conf.len != 0 && key_conf.len == value_conf.len &&
		    memcmp(key_conf.str, value_conf.str, key_conf.len) == 0) {
			btree->huffman_value = btree->huffman_key;
			return (0);
		}
	}
	if (strncasecmp(value_conf.str, "english", value_conf.len) == 0)
		WT_RET(__wt_huffman_open(session, __wt_huffman_ascii_english,
		    sizeof(__wt_huffman_ascii_english), &btree->huffman_value));
	else {
		WT_RET(__wt_huffman_read(session,
		    &value_conf, &huffman_table, &huffman_table_entries));
		ret = __wt_huffman_open(session, huffman_table,
		    huffman_table_entries, &btree->huffman_value);
		__wt_free(session, huffman_table);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

/*
 * __wt_huffman_read --
 *	Read a Huffman table from a file.
 */
static int
__wt_huffman_read(
    SESSION *session, WT_CONFIG_ITEM *ip, uint8_t **tablep, u_int *entriesp)
{
	FILE *fp;
	u_int byte, entries, freq, lineno, max;
	uint16_t *p16;
	uint8_t *p8;
	int ret;
	char *file;
	void *table;

	ret = 0;
	table = NULL;

	/*
	 * UTF-8 table is 256 bytes, with a range of 0-255.
	 * UTF-16 is 128KB (2 * 65536) bytes, with a range of 0-65535.
	 */
	if (strncasecmp(ip->str, "utf8", 4) == 0) {
		max = 255;
		entries = 256;
		WT_RET(__wt_calloc(session, entries, sizeof(uint8_t), &table));

		if (ip->len == 4)
			goto nofile;
		WT_ERR(__wt_calloc_def(session, ip->len, &file));
		memcpy(file, ip->str + 4, ip->len - 4);
		fp = fopen(file, "r");
		__wt_free(session, file);
		if (fp == NULL)
			goto nofile;

		for (p8 = table, lineno = 1;
		    (ret = fscanf(fp, "%i %i\n", &byte, &freq)) != EOF;
		    p8[byte] = (uint8_t)freq, ++lineno) {
			if (ret != 2 || byte > max || freq > max)
				goto corrupt;
			if (p8[byte] != 0)
				goto repeated;
		}
	} else if (strncasecmp(ip->str, "utf16", 5) == 0) {
		max = 65535;
		entries = 65536;
		WT_RET(__wt_calloc(session, entries, sizeof(uint16_t), &table));

		if (ip->len == 5)
			goto nofile;
		WT_ERR(__wt_calloc_def(session, ip->len, &file));
		memcpy(file, ip->str + 5, ip->len - 5);
		fp = fopen(file, "r");
		__wt_free(session, file);
		if (fp == NULL) {
nofile:			ret = errno == 0 ? WT_ERROR : errno;
			__wt_err(session, ret,
			    "unable to read Huffman table file %.*s",
			    (int)ip->len, ip->str);
			goto err;
		}

		for (p16 = table, lineno = 1;
		    (ret = fscanf(fp, "%i %i\n", &byte, &freq)) != EOF;
		    p16[byte] = (uint16_t)freq, ++lineno) {
			if (ret != 2 || byte > max || freq > max) {
corrupt:			__wt_errx(session,
				    "line %lu of Huffman table file %.*s is "
				    "corrupted: expected an unsigned integer "
				    "between 0 and %lu",
				    (u_long)lineno,
				    (int)ip->len, ip->str, (u_long)max);
				goto err;
			}
			if (p16[byte] != 0) {
repeated:			__wt_errx(session,
				    "line %lu of Huffman table file %.*s has a "
				    "repeated entry for byte value %#lx",
				    (u_long)lineno,
				    (int)ip->len, ip->str, (u_long)byte);
				goto err;
			}
		}
	} else {
		__wt_errx(session,
		    "unknown huffman configuration value %.*s",
		    (int)ip->len, ip->str);
		goto err;
	}

	*entriesp = entries;
	*tablep = table;
	return (0);

err:	if (table != NULL)
		__wt_free(session, table);
	if (ret == 0)
		ret = WT_ERROR;
	return (ret);
}

/*
 * __wt_btree_huffman_close --
 *	Close the Huffman tables.
 */
void
__wt_btree_huffman_close(SESSION *session)
{
	BTREE *btree;

	btree = session->btree;

	if (btree->huffman_key != NULL) {
		/* Key and data may use the same table, only close it once. */
		if (btree->huffman_value == btree->huffman_key)
			btree->huffman_value = NULL;
		__wt_huffman_close(session, btree->huffman_key);
		btree->huffman_key = NULL;
	}
	if (btree->huffman_value != NULL) {
		__wt_huffman_close(session, btree->huffman_value);
		btree->huffman_value = NULL;
	}
}
