/* DO NOT EDIT: automatically built by dist/api.py. */

#include "wt_internal.h"

static int __wt_api_btree_btree_compare_get(
	BTREE *btree,
	int (**btree_compare)(BTREE *, const WT_ITEM *, const WT_ITEM *));
static int __wt_api_btree_btree_compare_get(
	BTREE *btree,
	int (**btree_compare)(BTREE *, const WT_ITEM *, const WT_ITEM *))
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_COMPARE_GET);
	*btree_compare = btree->btree_compare;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_compare_int_get(
	BTREE *btree,
	int *btree_compare_int);
static int __wt_api_btree_btree_compare_int_get(
	BTREE *btree,
	int *btree_compare_int)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_COMPARE_INT_GET);
	*btree_compare_int = btree->btree_compare_int;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_compare_int_set(
	BTREE *btree,
	int btree_compare_int);
static int __wt_api_btree_btree_compare_int_set(
	BTREE *btree,
	int btree_compare_int)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	WT_RET((__wt_btree_btree_compare_int_set_verify(
	    btree, btree_compare_int)));
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_COMPARE_INT_SET);
	btree->btree_compare_int = btree_compare_int;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_compare_set(
	BTREE *btree,
	int (*btree_compare)(BTREE *, const WT_ITEM *, const WT_ITEM *));
static int __wt_api_btree_btree_compare_set(
	BTREE *btree,
	int (*btree_compare)(BTREE *, const WT_ITEM *, const WT_ITEM *))
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_COMPARE_SET);
	btree->btree_compare = btree_compare;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_itemsize_get(
	BTREE *btree,
	uint32_t *intlitemsize,
	uint32_t *leafitemsize);
static int __wt_api_btree_btree_itemsize_get(
	BTREE *btree,
	uint32_t *intlitemsize,
	uint32_t *leafitemsize)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_ITEMSIZE_GET);
	*intlitemsize = btree->intlitemsize;
	*leafitemsize = btree->leafitemsize;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_itemsize_set(
	BTREE *btree,
	uint32_t intlitemsize,
	uint32_t leafitemsize);
static int __wt_api_btree_btree_itemsize_set(
	BTREE *btree,
	uint32_t intlitemsize,
	uint32_t leafitemsize)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_ITEMSIZE_SET);
	btree->intlitemsize = intlitemsize;
	btree->leafitemsize = leafitemsize;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_pagesize_get(
	BTREE *btree,
	uint32_t *allocsize,
	uint32_t *intlmin,
	uint32_t *intlmax,
	uint32_t *leafmin,
	uint32_t *leafmax);
static int __wt_api_btree_btree_pagesize_get(
	BTREE *btree,
	uint32_t *allocsize,
	uint32_t *intlmin,
	uint32_t *intlmax,
	uint32_t *leafmin,
	uint32_t *leafmax)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_PAGESIZE_GET);
	*allocsize = btree->allocsize;
	*intlmin = btree->intlmin;
	*intlmax = btree->intlmax;
	*leafmin = btree->leafmin;
	*leafmax = btree->leafmax;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_btree_pagesize_set(
	BTREE *btree,
	uint32_t allocsize,
	uint32_t intlmin,
	uint32_t intlmax,
	uint32_t leafmin,
	uint32_t leafmax);
static int __wt_api_btree_btree_pagesize_set(
	BTREE *btree,
	uint32_t allocsize,
	uint32_t intlmin,
	uint32_t intlmax,
	uint32_t leafmin,
	uint32_t leafmax)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_BTREE_PAGESIZE_SET);
	btree->allocsize = allocsize;
	btree->intlmin = intlmin;
	btree->intlmax = intlmax;
	btree->leafmin = leafmin;
	btree->leafmax = leafmax;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_bulk_load(
	BTREE *btree,
	void (*progress)(const char *, uint64_t),
	int (*cb)(BTREE *, WT_ITEM **, WT_ITEM **));
static int __wt_api_btree_bulk_load(
	BTREE *btree,
	void (*progress)(const char *, uint64_t),
	int (*cb)(BTREE *, WT_ITEM **, WT_ITEM **))
{
	const char *method_name = "BTREE.bulk_load";
	CONNECTION *connection = btree->conn;
	SESSION *session = NULL;
	int islocal;
	int ret;

	WT_DB_RDONLY(session, btree, method_name);
	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_STAT_INCR(connection->method_stats, BTREE_BULK_LOAD);
	ret = __wt_btree_bulk_load(session, progress, cb);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_close(
	BTREE *btree,
	SESSION *session,
	uint32_t flags);
static int __wt_api_btree_close(
	BTREE *btree,
	SESSION *session,
	uint32_t flags)
{
	const char *method_name = "BTREE.close";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_CLOSE);
	WT_STAT_INCR(connection->method_stats, BTREE_CLOSE);
	ret = __wt_btree_close(session, flags);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_col_del(
	BTREE *btree,
	SESSION *session,
	uint64_t recno,
	uint32_t flags);
static int __wt_api_btree_col_del(
	BTREE *btree,
	SESSION *session,
	uint64_t recno,
	uint32_t flags)
{
	const char *method_name = "BTREE.col_del";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_DB_COL_ONLY(session, btree, method_name);
	WT_DB_RDONLY(session, btree, method_name);
	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_COL_DEL);
	WT_STAT_INCR(connection->method_stats, BTREE_COL_DEL);
	while ((ret = __wt_btree_col_del(session, recno)) == WT_RESTART)
		WT_STAT_INCR(connection->method_stats, BTREE_COL_DEL_RESTART);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_col_put(
	BTREE *btree,
	SESSION *session,
	uint64_t recno,
	WT_ITEM *value,
	uint32_t flags);
static int __wt_api_btree_col_put(
	BTREE *btree,
	SESSION *session,
	uint64_t recno,
	WT_ITEM *value,
	uint32_t flags)
{
	const char *method_name = "BTREE.col_put";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_DB_COL_ONLY(session, btree, method_name);
	WT_DB_RDONLY(session, btree, method_name);
	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_COL_PUT);
	WT_STAT_INCR(connection->method_stats, BTREE_COL_PUT);
	while ((ret = __wt_btree_col_put(session, recno, value)) == WT_RESTART)
		WT_STAT_INCR(connection->method_stats, BTREE_COL_PUT_RESTART);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_column_set(
	BTREE *btree,
	uint32_t fixed_len,
	const char *dictionary,
	uint32_t flags);
static int __wt_api_btree_column_set(
	BTREE *btree,
	uint32_t fixed_len,
	const char *dictionary,
	uint32_t flags)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;

	WT_CONN_FCHK(connection, "BTREE.column_set",
	    flags, WT_APIMASK_BTREE_COLUMN_SET);

	WT_RET((__wt_btree_column_set_verify(
	    btree, fixed_len, dictionary, flags)));
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_COLUMN_SET);
	btree->fixed_len = fixed_len;
	btree->dictionary = dictionary;
	F_SET(btree, flags);
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_btree_dump(
	BTREE *btree,
	FILE *stream,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_btree_dump(
	BTREE *btree,
	FILE *stream,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "BTREE.dump";
	CONNECTION *connection = btree->conn;
	SESSION *session = NULL;
	int islocal;
	int ret;

	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_DUMP);
	WT_STAT_INCR(connection->method_stats, BTREE_DUMP);
	ret = __wt_btree_dump(session, stream, progress, flags);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_huffman_set(
	BTREE *btree,
	uint8_t const *huffman_table,
	u_int huffman_table_size,
	uint32_t huffman_flags);
static int __wt_api_btree_huffman_set(
	BTREE *btree,
	uint8_t const *huffman_table,
	u_int huffman_table_size,
	uint32_t huffman_flags)
{
	CONNECTION *connection = btree->conn;
	SESSION *session = &connection->default_session;
	int ret;

	WT_CONN_FCHK(connection, "BTREE.huffman_set",
	    huffman_flags, WT_APIMASK_BTREE_HUFFMAN_SET);

	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, BTREE_HUFFMAN_SET);
	ret = __wt_btree_huffman_set(
	    btree, huffman_table, huffman_table_size, huffman_flags);
	__wt_unlock(session, connection->mtx);
	return (ret);
}

static int __wt_api_btree_open(
	BTREE *btree,
	SESSION *session,
	const char *name,
	mode_t mode,
	uint32_t flags);
static int __wt_api_btree_open(
	BTREE *btree,
	SESSION *session,
	const char *name,
	mode_t mode,
	uint32_t flags)
{
	const char *method_name = "BTREE.open";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_OPEN);
	WT_STAT_INCR(connection->method_stats, BTREE_OPEN);
	ret = __wt_btree_open(session, name, mode, flags);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_row_del(
	BTREE *btree,
	SESSION *session,
	WT_ITEM *key,
	uint32_t flags);
static int __wt_api_btree_row_del(
	BTREE *btree,
	SESSION *session,
	WT_ITEM *key,
	uint32_t flags)
{
	const char *method_name = "BTREE.row_del";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_DB_ROW_ONLY(session, btree, method_name);
	WT_DB_RDONLY(session, btree, method_name);
	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_ROW_DEL);
	WT_STAT_INCR(connection->method_stats, BTREE_ROW_DEL);
	while ((ret = __wt_btree_row_del(session, key)) == WT_RESTART)
		WT_STAT_INCR(connection->method_stats, BTREE_ROW_DEL_RESTART);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_row_put(
	BTREE *btree,
	SESSION *session,
	WT_ITEM *key,
	WT_ITEM *value,
	uint32_t flags);
static int __wt_api_btree_row_put(
	BTREE *btree,
	SESSION *session,
	WT_ITEM *key,
	WT_ITEM *value,
	uint32_t flags)
{
	const char *method_name = "BTREE.row_put";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_DB_ROW_ONLY(session, btree, method_name);
	WT_DB_RDONLY(session, btree, method_name);
	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_ROW_PUT);
	WT_STAT_INCR(connection->method_stats, BTREE_ROW_PUT);
	while ((ret = __wt_btree_row_put(session, key, value)) == WT_RESTART)
		WT_STAT_INCR(connection->method_stats, BTREE_ROW_PUT_RESTART);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_salvage(
	BTREE *btree,
	SESSION *session,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_btree_salvage(
	BTREE *btree,
	SESSION *session,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "BTREE.salvage";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_SALVAGE);
	WT_STAT_INCR(connection->method_stats, BTREE_SALVAGE);
	ret = __wt_btree_salvage(session, progress);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_stat_clear(
	BTREE *btree,
	uint32_t flags);
static int __wt_api_btree_stat_clear(
	BTREE *btree,
	uint32_t flags)
{
	const char *method_name = "BTREE.stat_clear";
	CONNECTION *connection = btree->conn;
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_STAT_CLEAR);
	WT_STAT_INCR(connection->method_stats, BTREE_STAT_CLEAR);
	ret = __wt_btree_stat_clear(btree);
	return (ret);
}

static int __wt_api_btree_stat_print(
	BTREE *btree,
	FILE *stream,
	uint32_t flags);
static int __wt_api_btree_stat_print(
	BTREE *btree,
	FILE *stream,
	uint32_t flags)
{
	const char *method_name = "BTREE.stat_print";
	CONNECTION *connection = btree->conn;
	SESSION *session = NULL;
	int islocal;
	int ret;

	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_STAT_PRINT);
	WT_STAT_INCR(connection->method_stats, BTREE_STAT_PRINT);
	ret = __wt_btree_stat_print(session, stream);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_sync(
	BTREE *btree,
	SESSION *session,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_btree_sync(
	BTREE *btree,
	SESSION *session,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "BTREE.sync";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_DB_RDONLY(session, btree, method_name);
	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_SYNC);
	WT_STAT_INCR(connection->method_stats, BTREE_SYNC);
	ret = __wt_btree_sync(session, progress, flags);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_btree_verify(
	BTREE *btree,
	SESSION *session,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_btree_verify(
	BTREE *btree,
	SESSION *session,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "BTREE.verify";
	CONNECTION *connection = btree->conn;
	int islocal;
	int ret;

	WT_RET(__wt_session_api_set(connection, method_name, btree, &session, &islocal));
	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_BTREE_VERIFY);
	WT_STAT_INCR(connection->method_stats, BTREE_VERIFY);
	ret = __wt_btree_verify(session, progress);
	WT_TRET(__wt_session_api_clr(session, method_name, islocal));
	return (ret);
}

static int __wt_api_connection_btree(
	CONNECTION *connection,
	uint32_t flags,
	BTREE **btreep);
static int __wt_api_connection_btree(
	CONNECTION *connection,
	uint32_t flags,
	BTREE **btreep)
{
	const char *method_name = "CONNECTION.btree";
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_BTREE);
	WT_STAT_INCR(connection->method_stats, CONNECTION_BTREE);
	ret = __wt_connection_btree(connection, btreep);
	return (ret);
}

static int __wt_api_connection_cache_size_get(
	CONNECTION *connection,
	uint32_t *cache_size);
static int __wt_api_connection_cache_size_get(
	CONNECTION *connection,
	uint32_t *cache_size)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_CACHE_SIZE_GET);
	*cache_size = connection->cache_size;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_cache_size_set(
	CONNECTION *connection,
	uint32_t cache_size);
static int __wt_api_connection_cache_size_set(
	CONNECTION *connection,
	uint32_t cache_size)
{
	SESSION *session = &connection->default_session;
	WT_RET((__wt_connection_cache_size_set_verify(
	    connection, cache_size)));
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_CACHE_SIZE_SET);
	connection->cache_size = cache_size;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_close(
	CONNECTION *connection,
	uint32_t flags);
static int __wt_api_connection_close(
	CONNECTION *connection,
	uint32_t flags)
{
	const char *method_name = "CONNECTION.close";
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_CLOSE);
	WT_STAT_INCR(connection->method_stats, CONNECTION_CLOSE);
	ret = __wt_connection_close(connection);
	return (ret);
}

static int __wt_api_connection_data_update_max_get(
	CONNECTION *connection,
	uint32_t *data_update_max);
static int __wt_api_connection_data_update_max_get(
	CONNECTION *connection,
	uint32_t *data_update_max)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_DATA_UPDATE_MAX_GET);
	*data_update_max = connection->data_update_max;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_data_update_max_set(
	CONNECTION *connection,
	uint32_t data_update_max);
static int __wt_api_connection_data_update_max_set(
	CONNECTION *connection,
	uint32_t data_update_max)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_DATA_UPDATE_MAX_SET);
	connection->data_update_max = data_update_max;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_data_update_min_get(
	CONNECTION *connection,
	uint32_t *data_update_min);
static int __wt_api_connection_data_update_min_get(
	CONNECTION *connection,
	uint32_t *data_update_min)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_DATA_UPDATE_MIN_GET);
	*data_update_min = connection->data_update_min;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_data_update_min_set(
	CONNECTION *connection,
	uint32_t data_update_min);
static int __wt_api_connection_data_update_min_set(
	CONNECTION *connection,
	uint32_t data_update_min)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_DATA_UPDATE_MIN_SET);
	connection->data_update_min = data_update_min;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_hazard_size_get(
	CONNECTION *connection,
	uint32_t *hazard_size);
static int __wt_api_connection_hazard_size_get(
	CONNECTION *connection,
	uint32_t *hazard_size)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_HAZARD_SIZE_GET);
	*hazard_size = connection->hazard_size;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_hazard_size_set(
	CONNECTION *connection,
	uint32_t hazard_size);
static int __wt_api_connection_hazard_size_set(
	CONNECTION *connection,
	uint32_t hazard_size)
{
	SESSION *session = &connection->default_session;
	WT_RET((__wt_connection_hazard_size_set_verify(
	    connection, hazard_size)));
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_HAZARD_SIZE_SET);
	connection->hazard_size = hazard_size;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_msgcall_get(
	CONNECTION *connection,
	void (**msgcall)(const CONNECTION *, const char *));
static int __wt_api_connection_msgcall_get(
	CONNECTION *connection,
	void (**msgcall)(const CONNECTION *, const char *))
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_MSGCALL_GET);
	*msgcall = connection->msgcall;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_msgcall_set(
	CONNECTION *connection,
	void (*msgcall)(const CONNECTION *, const char *));
static int __wt_api_connection_msgcall_set(
	CONNECTION *connection,
	void (*msgcall)(const CONNECTION *, const char *))
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_MSGCALL_SET);
	connection->msgcall = msgcall;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_msgfile_get(
	CONNECTION *connection,
	FILE **msgfile);
static int __wt_api_connection_msgfile_get(
	CONNECTION *connection,
	FILE **msgfile)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_MSGFILE_GET);
	*msgfile = connection->msgfile;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_msgfile_set(
	CONNECTION *connection,
	FILE *msgfile);
static int __wt_api_connection_msgfile_set(
	CONNECTION *connection,
	FILE *msgfile)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_MSGFILE_SET);
	connection->msgfile = msgfile;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_open(
	CONNECTION *connection,
	const char *home,
	mode_t mode,
	uint32_t flags);
static int __wt_api_connection_open(
	CONNECTION *connection,
	const char *home,
	mode_t mode,
	uint32_t flags)
{
	const char *method_name = "CONNECTION.open";
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_OPEN);
	WT_STAT_INCR(connection->method_stats, CONNECTION_OPEN);
	ret = __wt_connection_open(connection, home, mode);
	return (ret);
}

static int __wt_api_connection_session(
	CONNECTION *connection,
	uint32_t flags,
	SESSION **sessionp);
static int __wt_api_connection_session(
	CONNECTION *connection,
	uint32_t flags,
	SESSION **sessionp)
{
	const char *method_name = "CONNECTION.session";
	SESSION *session = &connection->default_session;
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_SESSION);
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_SESSION);
	ret = __wt_connection_session(connection, sessionp);
	__wt_unlock(session, connection->mtx);
	return (ret);
}

static int __wt_api_connection_session_size_get(
	CONNECTION *connection,
	uint32_t *session_size);
static int __wt_api_connection_session_size_get(
	CONNECTION *connection,
	uint32_t *session_size)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_SESSION_SIZE_GET);
	*session_size = connection->session_size;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_session_size_set(
	CONNECTION *connection,
	uint32_t session_size);
static int __wt_api_connection_session_size_set(
	CONNECTION *connection,
	uint32_t session_size)
{
	SESSION *session = &connection->default_session;
	WT_RET((__wt_connection_session_size_set_verify(
	    connection, session_size)));
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_SESSION_SIZE_SET);
	connection->session_size = session_size;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_stat_clear(
	CONNECTION *connection,
	uint32_t flags);
static int __wt_api_connection_stat_clear(
	CONNECTION *connection,
	uint32_t flags)
{
	const char *method_name = "CONNECTION.stat_clear";
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_STAT_CLEAR);
	WT_STAT_INCR(connection->method_stats, CONNECTION_STAT_CLEAR);
	ret = __wt_connection_stat_clear(connection);
	return (ret);
}

static int __wt_api_connection_stat_print(
	CONNECTION *connection,
	FILE *stream,
	uint32_t flags);
static int __wt_api_connection_stat_print(
	CONNECTION *connection,
	FILE *stream,
	uint32_t flags)
{
	const char *method_name = "CONNECTION.stat_print";
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_STAT_PRINT);
	WT_STAT_INCR(connection->method_stats, CONNECTION_STAT_PRINT);
	ret = __wt_connection_stat_print(connection, stream);
	return (ret);
}

static int __wt_api_connection_sync(
	CONNECTION *connection,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_connection_sync(
	CONNECTION *connection,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "CONNECTION.sync";
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_CONNECTION_SYNC);
	WT_STAT_INCR(connection->method_stats, CONNECTION_SYNC);
	ret = __wt_connection_sync(connection, progress);
	return (ret);
}

static int __wt_api_connection_verbose_get(
	CONNECTION *connection,
	uint32_t *verbose);
static int __wt_api_connection_verbose_get(
	CONNECTION *connection,
	uint32_t *verbose)
{
	SESSION *session = &connection->default_session;
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_VERBOSE_GET);
	*verbose = connection->verbose;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_connection_verbose_set(
	CONNECTION *connection,
	uint32_t verbose);
static int __wt_api_connection_verbose_set(
	CONNECTION *connection,
	uint32_t verbose)
{
	SESSION *session = &connection->default_session;
	WT_RET((__wt_connection_verbose_set_verify(
	    connection, verbose)));
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, CONNECTION_VERBOSE_SET);
	connection->verbose = verbose;
	__wt_unlock(session, connection->mtx);
	return (0);
}

static int __wt_api_session_close(
	SESSION *session,
	uint32_t flags);
static int __wt_api_session_close(
	SESSION *session,
	uint32_t flags)
{
	const char *method_name = "SESSION.close";
	CONNECTION *connection = S2C(session);
	int ret;

	WT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_SESSION_CLOSE);
	__wt_lock(session, connection->mtx);
	WT_STAT_INCR(connection->method_stats, SESSION_CLOSE);
	ret = __wt_session_close(session);
	session = &connection->default_session;
	__wt_unlock(session, connection->mtx);
	return (ret);
}

void
__wt_methods_btree_config_default(BTREE *btree)
{
	btree->btree_compare = __wt_bt_lex_compare;
}

void
__wt_methods_btree_lockout(BTREE *btree)
{
	btree->btree_compare_get = (int (*)
	    (BTREE *, int (**)(BTREE *, const WT_ITEM *, const WT_ITEM *)))
	    __wt_btree_lockout;
	btree->btree_compare_int_get = (int (*)
	    (BTREE *, int *))
	    __wt_btree_lockout;
	btree->btree_compare_int_set = (int (*)
	    (BTREE *, int ))
	    __wt_btree_lockout;
	btree->btree_compare_set = (int (*)
	    (BTREE *, int (*)(BTREE *, const WT_ITEM *, const WT_ITEM *)))
	    __wt_btree_lockout;
	btree->btree_itemsize_get = (int (*)
	    (BTREE *, uint32_t *, uint32_t *))
	    __wt_btree_lockout;
	btree->btree_itemsize_set = (int (*)
	    (BTREE *, uint32_t , uint32_t ))
	    __wt_btree_lockout;
	btree->btree_pagesize_get = (int (*)
	    (BTREE *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *))
	    __wt_btree_lockout;
	btree->btree_pagesize_set = (int (*)
	    (BTREE *, uint32_t , uint32_t , uint32_t , uint32_t , uint32_t ))
	    __wt_btree_lockout;
	btree->bulk_load = (int (*)
	    (BTREE *, void (*)(const char *, uint64_t), int (*)(BTREE *, WT_ITEM **, WT_ITEM **)))
	    __wt_btree_lockout;
	btree->col_del = (int (*)
	    (BTREE *, SESSION *, uint64_t , uint32_t ))
	    __wt_btree_lockout;
	btree->col_put = (int (*)
	    (BTREE *, SESSION *, uint64_t , WT_ITEM *, uint32_t ))
	    __wt_btree_lockout;
	btree->column_set = (int (*)
	    (BTREE *, uint32_t , const char *, uint32_t ))
	    __wt_btree_lockout;
	btree->dump = (int (*)
	    (BTREE *, FILE *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_btree_lockout;
	btree->huffman_set = (int (*)
	    (BTREE *, uint8_t const *, u_int , uint32_t ))
	    __wt_btree_lockout;
	btree->open = (int (*)
	    (BTREE *, SESSION *, const char *, mode_t , uint32_t ))
	    __wt_btree_lockout;
	btree->row_del = (int (*)
	    (BTREE *, SESSION *, WT_ITEM *, uint32_t ))
	    __wt_btree_lockout;
	btree->row_put = (int (*)
	    (BTREE *, SESSION *, WT_ITEM *, WT_ITEM *, uint32_t ))
	    __wt_btree_lockout;
	btree->salvage = (int (*)
	    (BTREE *, SESSION *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_btree_lockout;
	btree->stat_clear = (int (*)
	    (BTREE *, uint32_t ))
	    __wt_btree_lockout;
	btree->stat_print = (int (*)
	    (BTREE *, FILE *, uint32_t ))
	    __wt_btree_lockout;
	btree->sync = (int (*)
	    (BTREE *, SESSION *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_btree_lockout;
	btree->verify = (int (*)
	    (BTREE *, SESSION *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_btree_lockout;
}

void
__wt_methods_btree_init_transition(BTREE *btree)
{
	btree->btree_compare_get = __wt_api_btree_btree_compare_get;
	btree->btree_compare_int_get = __wt_api_btree_btree_compare_int_get;
	btree->btree_compare_int_set = __wt_api_btree_btree_compare_int_set;
	btree->btree_compare_set = __wt_api_btree_btree_compare_set;
	btree->btree_itemsize_get = __wt_api_btree_btree_itemsize_get;
	btree->btree_itemsize_set = __wt_api_btree_btree_itemsize_set;
	btree->btree_pagesize_get = __wt_api_btree_btree_pagesize_get;
	btree->btree_pagesize_set = __wt_api_btree_btree_pagesize_set;
	btree->close = __wt_api_btree_close;
	btree->column_set = __wt_api_btree_column_set;
	btree->huffman_set = __wt_api_btree_huffman_set;
	btree->open = __wt_api_btree_open;
}

void
__wt_methods_btree_open_transition(BTREE *btree)
{
	btree->btree_compare_int_set = (int (*)
	    (BTREE *, int ))
	    __wt_btree_lockout;
	btree->btree_compare_set = (int (*)
	    (BTREE *, int (*)(BTREE *, const WT_ITEM *, const WT_ITEM *)))
	    __wt_btree_lockout;
	btree->btree_itemsize_set = (int (*)
	    (BTREE *, uint32_t , uint32_t ))
	    __wt_btree_lockout;
	btree->btree_pagesize_set = (int (*)
	    (BTREE *, uint32_t , uint32_t , uint32_t , uint32_t , uint32_t ))
	    __wt_btree_lockout;
	btree->column_set = (int (*)
	    (BTREE *, uint32_t , const char *, uint32_t ))
	    __wt_btree_lockout;
	btree->huffman_set = (int (*)
	    (BTREE *, uint8_t const *, u_int , uint32_t ))
	    __wt_btree_lockout;
	btree->bulk_load = __wt_api_btree_bulk_load;
	btree->col_del = __wt_api_btree_col_del;
	btree->col_put = __wt_api_btree_col_put;
	btree->dump = __wt_api_btree_dump;
	btree->row_del = __wt_api_btree_row_del;
	btree->row_put = __wt_api_btree_row_put;
	btree->salvage = __wt_api_btree_salvage;
	btree->stat_clear = __wt_api_btree_stat_clear;
	btree->stat_print = __wt_api_btree_stat_print;
	btree->sync = __wt_api_btree_sync;
	btree->verify = __wt_api_btree_verify;
}

void
__wt_methods_connection_config_default(CONNECTION *connection)
{
	connection->cache_size = 20;
	connection->data_update_max = 32 * 1024;
	connection->data_update_min = 8 * 1024;
	connection->hazard_size = 15;
	connection->session_size = 50;
}

void
__wt_methods_connection_lockout(CONNECTION *connection)
{
	connection->btree = (int (*)
	    (CONNECTION *, uint32_t , BTREE **))
	    __wt_connection_lockout;
	connection->cache_size_get = (int (*)
	    (CONNECTION *, uint32_t *))
	    __wt_connection_lockout;
	connection->cache_size_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->data_update_max_get = (int (*)
	    (CONNECTION *, uint32_t *))
	    __wt_connection_lockout;
	connection->data_update_max_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->data_update_min_get = (int (*)
	    (CONNECTION *, uint32_t *))
	    __wt_connection_lockout;
	connection->data_update_min_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->hazard_size_get = (int (*)
	    (CONNECTION *, uint32_t *))
	    __wt_connection_lockout;
	connection->hazard_size_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->msgcall_get = (int (*)
	    (CONNECTION *, void (**)(const CONNECTION *, const char *)))
	    __wt_connection_lockout;
	connection->msgcall_set = (int (*)
	    (CONNECTION *, void (*)(const CONNECTION *, const char *)))
	    __wt_connection_lockout;
	connection->msgfile_get = (int (*)
	    (CONNECTION *, FILE **))
	    __wt_connection_lockout;
	connection->msgfile_set = (int (*)
	    (CONNECTION *, FILE *))
	    __wt_connection_lockout;
	connection->open = (int (*)
	    (CONNECTION *, const char *, mode_t , uint32_t ))
	    __wt_connection_lockout;
	connection->session = (int (*)
	    (CONNECTION *, uint32_t , SESSION **))
	    __wt_connection_lockout;
	connection->session_size_get = (int (*)
	    (CONNECTION *, uint32_t *))
	    __wt_connection_lockout;
	connection->session_size_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->stat_clear = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->stat_print = (int (*)
	    (CONNECTION *, FILE *, uint32_t ))
	    __wt_connection_lockout;
	connection->sync = (int (*)
	    (CONNECTION *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_connection_lockout;
	connection->verbose_get = (int (*)
	    (CONNECTION *, uint32_t *))
	    __wt_connection_lockout;
	connection->verbose_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
}

void
__wt_methods_connection_open_transition(CONNECTION *connection)
{
	connection->cache_size_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->hazard_size_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->open = (int (*)
	    (CONNECTION *, const char *, mode_t , uint32_t ))
	    __wt_connection_lockout;
	connection->session_size_set = (int (*)
	    (CONNECTION *, uint32_t ))
	    __wt_connection_lockout;
	connection->btree = __wt_api_connection_btree;
	connection->session = __wt_api_connection_session;
	connection->sync = __wt_api_connection_sync;
}

void
__wt_methods_connection_init_transition(CONNECTION *connection)
{
	connection->cache_size_get = __wt_api_connection_cache_size_get;
	connection->cache_size_set = __wt_api_connection_cache_size_set;
	connection->close = __wt_api_connection_close;
	connection->data_update_max_get = __wt_api_connection_data_update_max_get;
	connection->data_update_max_set = __wt_api_connection_data_update_max_set;
	connection->data_update_min_get = __wt_api_connection_data_update_min_get;
	connection->data_update_min_set = __wt_api_connection_data_update_min_set;
	connection->hazard_size_get = __wt_api_connection_hazard_size_get;
	connection->hazard_size_set = __wt_api_connection_hazard_size_set;
	connection->msgcall_get = __wt_api_connection_msgcall_get;
	connection->msgcall_set = __wt_api_connection_msgcall_set;
	connection->msgfile_get = __wt_api_connection_msgfile_get;
	connection->msgfile_set = __wt_api_connection_msgfile_set;
	connection->open = __wt_api_connection_open;
	connection->session_size_get = __wt_api_connection_session_size_get;
	connection->session_size_set = __wt_api_connection_session_size_set;
	connection->stat_clear = __wt_api_connection_stat_clear;
	connection->stat_print = __wt_api_connection_stat_print;
	connection->verbose_get = __wt_api_connection_verbose_get;
	connection->verbose_set = __wt_api_connection_verbose_set;
}

void
__wt_methods_session_lockout(SESSION *session)
{
	WT_UNUSED(session);
}

void
__wt_methods_session_init_transition(SESSION *session)
{
	session->close = __wt_api_session_close;
}

