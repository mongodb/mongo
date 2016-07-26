/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
struct __wt_huffman_table {
	uint32_t symbol;
	uint32_t frequency;
};
static const struct __wt_huffman_table __wt_huffman_nytenglish[] = {
	/* nul */	{ 0x00,       0 },	/* For an escape character. */
	/*  ht */	{ 0x09, 5263779 },
	/*  sp */	{ 0x20, 8000000 },
	/*  !  */	{ 0x21,    2178 },
	/*  "  */	{ 0x22,  284671 },
	/*  #  */	{ 0x23,      10 },
	/*  $  */	{ 0x24,   51572 },
	/*  %  */	{ 0x25,    1993 },
	/*  &  */	{ 0x26,    6523 },
	/*  '  */	{ 0x27,  204497 },
	/*  (  */	{ 0x28,   53398 },
	/*  )  */	{ 0x29,   53735 },
	/*  *  */	{ 0x2a,   20716 },
	/*  +  */	{ 0x2b,     309 },
	/*  ,  */	{ 0x2c,  984969 },
	/*  -  */	{ 0x2d,  252302 },
	/*  .  */	{ 0x2e,  946136 },
	/*  /  */	{ 0x2f,    8161 },
	/*  0  */	{ 0x30,  546233 },
	/*  1  */	{ 0x31,  460946 },
	/*  2  */	{ 0x32,  333499 },
	/*  3  */	{ 0x33,  187606 },
	/*  4  */	{ 0x34,  192528 },
	/*  5  */	{ 0x35,  374413 },
	/*  6  */	{ 0x36,  153865 },
	/*  7  */	{ 0x37,  120094 },
	/*  8  */	{ 0x38,  182627 },
	/*  9  */	{ 0x39,  282364 },
	/*  :  */	{ 0x3a,   54036 },
	/*  ;  */	{ 0x3b,   36727 },
	/*  <  */	{ 0x3c,      82 },
	/*  =  */	{ 0x3d,      22 },
	/*  >  */	{ 0x3e,      83 },
	/*  ?  */	{ 0x3f,   12357 },
	/*  @  */	{ 0x40,       1 },
	/*  A  */	{ 0x41,  280937 },
	/*  B  */	{ 0x42,  169474 },
	/*  C  */	{ 0x43,  229363 },
	/*  D  */	{ 0x44,  129632 },
	/*  E  */	{ 0x45,  138443 },
	/*  F  */	{ 0x46,  100751 },
	/*  G  */	{ 0x47,   93212 },
	/*  H  */	{ 0x48,  123632 },
	/*  I  */	{ 0x49,  223312 },
	/*  J  */	{ 0x4a,   78706 },
	/*  K  */	{ 0x4b,   46580 },
	/*  L  */	{ 0x4c,  106984 },
	/*  M  */	{ 0x4d,  259474 },
	/*  N  */	{ 0x4e,  205409 },
	/*  O  */	{ 0x4f,  105700 },
	/*  P  */	{ 0x50,  144239 },
	/*  Q  */	{ 0x51,   11659 },
	/*  R  */	{ 0x52,  146448 },
	/*  S  */	{ 0x53,  304971 },
	/*  T  */	{ 0x54,  325462 },
	/*  U  */	{ 0x55,   57488 },
	/*  V  */	{ 0x56,   31053 },
	/*  W  */	{ 0x57,  107195 },
	/*  X  */	{ 0x58,    7578 },
	/*  Y  */	{ 0x59,   94297 },
	/*  Z  */	{ 0x5a,    5610 },
	/*  [  */	{ 0x5b,       1 },
	/*  \  */	{ 0x5c,       1 },
	/*  ]  */	{ 0x5d,       1 },
	/*  ^  */	{ 0x5e,       1 },
	/*  _  */	{ 0x5f,       1 },
	/*  `  */	{ 0x60,       1 },
	/*  a  */	{ 0x61, 5263779 },
	/*  b  */	{ 0x62,  866156 },
	/*  c  */	{ 0x63, 1960412 },
	/*  d  */	{ 0x64, 2369820 },
	/*  e  */	{ 0x65, 7741842 },
	/*  f  */	{ 0x66, 1296925 },
	/*  g  */	{ 0x67, 1206747 },
	/*  h  */	{ 0x68, 2955858 },
	/*  i  */	{ 0x69, 4527332 },
	/*  j  */	{ 0x6a,   65856 },
	/*  k  */	{ 0x6b,  460788 },
	/*  l  */	{ 0x6c, 2553152 },
	/*  m  */	{ 0x6d, 1467376 },
	/*  n  */	{ 0x6e, 4535545 },
	/*  o  */	{ 0x6f, 4729266 },
	/*  p  */	{ 0x70, 1255579 },
	/*  q  */	{ 0x71,   54221 },
	/*  r  */	{ 0x72, 4137949 },
	/*  s  */	{ 0x73, 4186210 },
	/*  t  */	{ 0x74, 5507692 },
	/*  u  */	{ 0x75, 1613323 },
	/*  v  */	{ 0x76,  653370 },
	/*  w  */	{ 0x77, 1015656 },
	/*  x  */	{ 0x78,  123577 },
	/*  y  */	{ 0x79, 1062040 },
	/*  z  */	{ 0x7a,   66423 },
	/*  {  */	{ 0x7b,       1 },
	/*  |  */	{ 0x7c,       1 },
	/*  }  */	{ 0x7d,       1 },
	/*  ~  */	{ 0x7e,       1 }
};

static int __wt_huffman_read(WT_SESSION_IMPL *,
    WT_CONFIG_ITEM *, struct __wt_huffman_table **, u_int *, u_int *);

/*
 * __huffman_confchk_file --
 *	Check for a Huffman configuration file and return the file name.
 */
static int
__huffman_confchk_file(WT_SESSION_IMPL *session,
    WT_CONFIG_ITEM *v, bool *is_utf8p, WT_FSTREAM **fsp)
{
	WT_FSTREAM *fs;
	WT_DECL_RET;
	size_t len;
	char *fname;

	/* Look for a prefix and file name. */
	len = 0;
	if (is_utf8p != NULL)
		*is_utf8p = 0;
	if (WT_PREFIX_MATCH(v->str, "utf8")) {
		if (is_utf8p != NULL)
			*is_utf8p = 1;
		len = strlen("utf8");
	} else if (WT_PREFIX_MATCH(v->str, "utf16"))
		len = strlen("utf16");
	if (len == 0 || len >= v->len)
		WT_RET_MSG(session, EINVAL,
		    "illegal Huffman configuration: %.*s", (int)v->len, v->str);

	/* Check the file exists. */
	WT_RET(__wt_strndup(session, v->str + len, v->len - len, &fname));
	WT_ERR(__wt_fopen(session, fname, WT_OPEN_FIXED, WT_STREAM_READ, &fs));

	/* Optionally return the file handle. */
	if (fsp == NULL)
		(void)__wt_fclose(session, &fs);
	else
		*fsp = fs;

err:	__wt_free(session, fname);

	return (ret);
}

/*
 * __huffman_confchk --
 *	Verify Huffman configuration.
 */
static int
__huffman_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *v)
{
	if (v->len == 0)
		return (0);

	/* Standard Huffman encodings, no work to be done. */
	if (WT_STRING_MATCH("english", v->str, v->len))
		return (0);
	if (WT_STRING_MATCH("none", v->str, v->len))
		return (0);

	return (__huffman_confchk_file(session, v, NULL, NULL));
}

/*
 * __wt_btree_huffman_open --
 *	Configure Huffman encoding for the tree.
 */
int
__wt_btree_huffman_open(WT_SESSION_IMPL *session)
{
	struct __wt_huffman_table *table;
	WT_BTREE *btree;
	WT_CONFIG_ITEM key_conf, value_conf;
	WT_DECL_RET;
	const char **cfg;
	u_int entries, numbytes;

	btree = S2BT(session);
	cfg = btree->dhandle->cfg;

	WT_RET(__wt_config_gets_none(session, cfg, "huffman_key", &key_conf));
	WT_RET(__huffman_confchk(session, &key_conf));
	WT_RET(
	    __wt_config_gets_none(session, cfg, "huffman_value", &value_conf));
	WT_RET(__huffman_confchk(session, &value_conf));
	if (key_conf.len == 0 && value_conf.len == 0)
		return (0);

	switch (btree->type) {		/* Check file type compatibility. */
	case BTREE_COL_FIX:
		WT_RET_MSG(session, EINVAL,
		    "fixed-size column-store files may not be Huffman encoded");
		/* NOTREACHED */
	case BTREE_COL_VAR:
		if (key_conf.len != 0)
			WT_RET_MSG(session, EINVAL,
			    "the keys of variable-length column-store files "
			    "may not be Huffman encoded");
		break;
	case BTREE_ROW:
		break;
	}

	if (key_conf.len == 0) {
		;
	} else if (strncmp(key_conf.str, "english", key_conf.len) == 0) {
		struct __wt_huffman_table
		    copy[WT_ELEMENTS(__wt_huffman_nytenglish)];

		memcpy(copy,
		    __wt_huffman_nytenglish, sizeof(__wt_huffman_nytenglish));
		WT_RET(__wt_huffman_open(
		    session, copy, WT_ELEMENTS(__wt_huffman_nytenglish),
		    1, &btree->huffman_key));

		/* Check for a shared key/value table. */
		if (value_conf.len != 0 && strncmp(
		    value_conf.str, "english", value_conf.len) == 0) {
			btree->huffman_value = btree->huffman_key;
			return (0);
		}
	} else {
		WT_RET(__wt_huffman_read(
		    session, &key_conf, &table, &entries, &numbytes));
		ret = __wt_huffman_open(
		    session, table, entries, numbytes, &btree->huffman_key);
		__wt_free(session, table);
		if (ret != 0)
			return (ret);

		/* Check for a shared key/value table. */
		if (value_conf.len != 0 && key_conf.len == value_conf.len &&
		    memcmp(key_conf.str, value_conf.str, key_conf.len) == 0) {
			btree->huffman_value = btree->huffman_key;
			return (0);
		}
	}

	if (value_conf.len == 0) {
		;
	} else if (strncmp(value_conf.str, "english", value_conf.len) == 0) {
		struct __wt_huffman_table
		    copy[WT_ELEMENTS(__wt_huffman_nytenglish)];

		memcpy(copy,
		    __wt_huffman_nytenglish, sizeof(__wt_huffman_nytenglish));
		WT_RET(__wt_huffman_open(
		    session, copy, WT_ELEMENTS(__wt_huffman_nytenglish),
		    1, &btree->huffman_value));
	} else {
		WT_RET(__wt_huffman_read(
		    session, &value_conf, &table, &entries, &numbytes));
		ret = __wt_huffman_open(
		    session, table, entries, numbytes, &btree->huffman_value);
		__wt_free(session, table);
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
__wt_huffman_read(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *ip,
    struct __wt_huffman_table **tablep, u_int *entriesp, u_int *numbytesp)
{
	struct __wt_huffman_table *table, *tp;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_FSTREAM *fs;
	int64_t symbol, frequency;
	u_int entries, lineno;
	int n;
	bool is_utf8;

	*tablep = NULL;
	*entriesp = *numbytesp = 0;

	fs = NULL;
	table = NULL;

	/*
	 * Try and open the backing file.
	 */
	WT_RET(__huffman_confchk_file(session, ip, &is_utf8, &fs));

	/*
	 * UTF-8 table is 256 bytes, with a range of 0-255.
	 * UTF-16 is 128KB (2 * 65536) bytes, with a range of 0-65535.
	 */
	if (is_utf8) {
		entries = UINT8_MAX;
		*numbytesp = 1;
		WT_ERR(__wt_calloc_def(session, entries, &table));
	} else {
		entries = UINT16_MAX;
		*numbytesp = 2;
		WT_ERR(__wt_calloc_def(session, entries, &table));
	}

	WT_ERR(__wt_scr_alloc(session, 0, &tmp));
	for (tp = table, lineno = 1;; ++tp, ++lineno) {
		WT_ERR(__wt_getline(session, fs, tmp));
		if (tmp->size == 0)
			break;
		n = sscanf(
		    tmp->data, "%" SCNi64 " %" SCNi64, &symbol, &frequency);
		/*
		 * Entries is 0-based, that is, there are (entries +1) possible
		 * values that can be configured. The line number is 1-based, so
		 * adjust the test for too many entries, and report (entries +1)
		 * in the error as the maximum possible number of entries.
		 */
		if (lineno > entries + 1)
			WT_ERR_MSG(session, EINVAL,
			    "Huffman table file %.*s is corrupted, "
			    "more than %" PRIu32 " entries",
			    (int)ip->len, ip->str, entries + 1);
		if (n != 2)
			WT_ERR_MSG(session, EINVAL,
			    "line %u of Huffman table file %.*s is corrupted: "
			    "expected two unsigned integral values",
			    lineno, (int)ip->len, ip->str);
		if (symbol < 0 || symbol > entries)
			WT_ERR_MSG(session, EINVAL,
			    "line %u of Huffman file %.*s is corrupted; "
			    "symbol %" PRId64 " not in range, maximum "
			    "value is %u",
			    lineno, (int)ip->len, ip->str, symbol, entries);
		if (frequency < 0 || frequency > UINT32_MAX)
			WT_ERR_MSG(session, EINVAL,
			    "line %u of Huffman file %.*s is corrupted; "
			    "frequency %" PRId64 " not in range, maximum "
			    "value is %" PRIu32,
			    lineno, (int)ip->len, ip->str, frequency,
			    (uint32_t)UINT32_MAX);

		tp->symbol = (uint32_t)symbol;
		tp->frequency = (uint32_t)frequency;
	}

	*entriesp = lineno - 1;
	*tablep = table;

	if (0) {
err:		__wt_free(session, table);
	}
	(void)__wt_fclose(session, &fs);

	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_btree_huffman_close --
 *	Close the Huffman tables.
 */
void
__wt_btree_huffman_close(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = S2BT(session);

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
