/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__find_next_col(WT_SESSION_IMPL *session,
    WT_TABLE *table, WT_CONFIG_ITEM *colname, int *cgnump, int *colnump)
{
	WT_BTREE *cgtree;
	WT_CONFIG conf;
	WT_CONFIG_ITEM cval, k, v;
	int cg, col, foundcg, foundcol, getnext;

	foundcg = foundcol = -1;

	getnext = 1;
	for (cg = 0; cg < WT_COLGROUPS(table); cg++) {
		if ((cgtree = table->colgroup[cg]) == NULL)
			continue;
		if (table->ncolgroups == 0)
			cval = table->colconf;
		else
			WT_RET(__wt_config_getones(session,
			    cgtree->config, "columns", &cval));
		WT_RET(__wt_config_subinit(session, &conf, &cval));
		for (col = 0;
		    __wt_config_next(&conf, &k, &v) == 0;
		    col++) {
			if (cg == *cgnump && col == *colnump)
				getnext = 1;
			if (getnext && k.len == colname->len &&
			    strncmp(colname->str, k.str, k.len) == 0) {
				foundcg = cg;
				foundcol = col;
				getnext = 0;
			}
		}
	}

	if (foundcg == -1)
		return (WT_NOTFOUND);

	*cgnump = foundcg;
	*colnump = foundcol;
	return (0);
}

/*
 * __wt_table_check --
 *	Make sure all columns appear in a column group.
 */
int
__wt_table_check(WT_SESSION_IMPL *session, WT_TABLE *table)
{
	WT_CONFIG conf;
	WT_CONFIG_ITEM k, v;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	int cg, col, i, ret;

	if (table->is_simple)
		return (0);

	/* Count the number of key columns. */
	WT_RET(__pack_init(session, &pack, table->key_format));
	table->nkey_columns = 0;
	while ((ret = __pack_next(&pack, &pv)) == 0)
		++table->nkey_columns;
	WT_ASSERT(session, ret == WT_NOTFOUND);

	/* Walk through the columns. */
	WT_RET(__wt_config_subinit(session, &conf, &table->colconf));

	/* Skip over the key columns. */
	for (i = 0; i < table->nkey_columns; i++)
		WT_RET(__wt_config_next(&conf, &k, &v));
	cg = col = 0;
	while ((ret = __wt_config_next(&conf, &k, &v)) == 0) {
		if (__find_next_col(session, table, &k, &cg, &col) == 0)
			continue;

		__wt_errx(session, "Column '%.*s' in table '%s' "
		    "does not appear in a column group",
		    (int)k.len, k.str, table->name);
		return (EINVAL);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}

/*
 * __wt_struct_plan --
 *	Given a table cursor containing a complete table, build the "projection
 *	plan" to distribute the columns to dependent stores.  A string
 *	representing the plan will be appended to the plan buffer.
 */
int
__wt_struct_plan(WT_SESSION_IMPL *session, WT_TABLE *table,
    const char *columns, size_t len, int value_only, WT_BUF *plan)
{
	WT_CONFIG conf;
	WT_CONFIG_ITEM k, v;
	int cg, col, current_cg, current_col, start_cg, start_col;
	int i, have_it;

	/* Work through the value columns by skipping over the key columns. */
	WT_RET(__wt_config_initn(session, &conf, columns, len));

	if (value_only)
		for (i = 0; i < table->nkey_columns; i++)
			WT_RET(__wt_config_next(&conf, &k, &v));

	current_cg = cg = 0;
	current_col = col = INT_MAX;
	while (__wt_config_next(&conf, &k, &v) == 0) {
		have_it = 0;

		while (__find_next_col(session, table, &k, &cg, &col) == 0 &&
		    (!have_it || cg != start_cg || col != start_col)) {
			/*
			 * First we move to the column.  If that is in a
			 * different column group to the last column we
			 * accessed, or before the last column in the same
			 * column group, or moving from the key to the value,
			 * we need to switch column groups or rewind.
			 */
			if (current_cg != cg || current_col > col ||
			    (current_col < table->nkey_columns &&
			    col >= table->nkey_columns)) {
				WT_ASSERT(session, !value_only ||
				    col >= table->nkey_columns);
				WT_RET(__wt_buf_sprintf(session,
				    plan, "%d%c", cg,
				    (col < table->nkey_columns) ?
				    WT_PROJ_KEY : WT_PROJ_VALUE));

				/*
				 * Set the current column group and column
				 * within the table.
				 */
				current_cg = cg;
				current_col = (col < table->nkey_columns) ? 0 :
				    table->nkey_columns;
			}
			/* Now move to the column we want. */
			if (current_col < col) {
				if (col - current_col > 1)
					WT_RET(__wt_buf_sprintf(session,
					    plan, "%d", col - current_col));
				WT_RET(__wt_buf_sprintf(session,
				    plan, "%c", WT_PROJ_SKIP));
			}
			/*
			 * Now copy the value in / out.  In the common case,
			 * where each value is used in one column, we do a
			 * "next" operation.  If the value is used again, we do
			 * a "reuse" operation to avoid making another copy.
			 */
			if (!have_it) {
				WT_RET(__wt_buf_sprintf(session,
				    plan, "%c", WT_PROJ_NEXT));

				start_cg = cg;
				start_col = col;
				have_it = 1;
			} else
				WT_RET(__wt_buf_sprintf(session,
				    plan, "%c", WT_PROJ_REUSE));
			current_col = col + 1;
		}
	}

	return (0);
}

static int
__find_column_format(WT_SESSION_IMPL *session,
    WT_TABLE *table, WT_CONFIG_ITEM *colname, int value_only, WT_PACK_VALUE *pv)
{
	WT_CONFIG conf;
	WT_CONFIG_ITEM k, v;
	WT_PACK pack;
	int inkey, ret;

	WT_RET(__wt_config_subinit(session, &conf, &table->colconf));
	WT_RET(__pack_init(session, &pack, table->key_format));
	inkey = 1;

	while ((ret = __wt_config_next(&conf, &k, &v)) == 0) {
		if ((ret = __pack_next(&pack, pv)) == WT_NOTFOUND && inkey) {
			ret = __pack_init(session, &pack, table->value_format);
			if (ret == 0)
				ret = __pack_next(&pack, pv);
			inkey = 0;
		}
		if (ret != 0)
			return (ret);

		if (k.len == colname->len &&
		    strncmp(colname->str, k.str, k.len) == 0) {
			if (value_only && inkey)
				return (EINVAL);
			return (0);
		}
	}

	return (ret);
}

/*
 * __wt_struct_reformat --
 *	Given a table and a list of columns (which could be values in a column
 *	group or index keys), calculate the resulting new format string.
 *	The result will be appended to the format buffer.
 */
int
__wt_struct_reformat(WT_SESSION_IMPL *session, WT_TABLE *table,
    const char *columns, size_t len, const char *extra_cols, int value_only,
    WT_BUF *format)
{
	WT_CONFIG config;
	WT_CONFIG_ITEM k, next_k, next_v;
	WT_PACK_VALUE pv;
	int have_next, ret;

	WT_RET(__wt_config_initn(session, &config, columns, len));
	WT_RET(__wt_config_next(&config, &next_k, &next_v));
	do {
		k = next_k;
		ret = __wt_config_next(&config, &next_k, &next_v);
		if (ret != 0 && ret != WT_NOTFOUND)
			return (ret);
		have_next = (ret == 0);

		if (!have_next && extra_cols != NULL) {
			WT_RET(__wt_config_init(session, &config, extra_cols));
			WT_RET(__wt_config_next(&config, &next_k, &next_v));
			have_next = 1;
			extra_cols = NULL;
		}

		if ((ret = __find_column_format(session,
		    table, &k, value_only, &pv)) != 0) {
			if (value_only && ret == EINVAL)
				__wt_errx(session,
				    "A column group cannot store key column "
				    "'%.*s' in its value", (int)k.len, k.str);
			else
				__wt_errx(session, "Column '%.*s' not found",
				    (int)k.len, k.str);
			return (EINVAL);
		}

		/*
		 * Check whether we're moving an unsized WT_ITEM from the end
		 * to the middle, or vice-versa.  This determines whether the
		 * size needs to be prepended.  This is the only case where the
		 * destination size can be larger than the source size.
		 */
		if (pv.type == 'u' && !pv.havesize && have_next)
			pv.type = 'U';
		else if (pv.type == 'U' && !have_next)
			pv.type = 'u';

		if (pv.havesize)
			WT_RET(__wt_buf_sprintf(session, format, "%d%c",
			    (int)pv.size, pv.type));
		else
			WT_RET(__wt_buf_sprintf(session, format, "%c",
			    pv.type));
	} while (have_next);

	return (0);
}
