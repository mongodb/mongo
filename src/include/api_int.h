/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*******************************************
 * Forward structure declarations -- not all are needed by wiredtiger.h, but
 * it's easier to keep them in one place and incrementally increase the risk
 * of stepping on the application's name space.
 *******************************************/
struct __dbt;			typedef struct __dbt DBT;
struct __db;			typedef struct __db DB;
struct __btree;			typedef struct __btree BTREE;
struct __env;			typedef struct __env ENV;
struct __ienv;			typedef struct __ienv IENV;

/*******************************************
 * Key/data structure -- a Data-Base Thang
 *******************************************/
struct __dbt {
	void    *data;			/* returned/specified data */
	uint32_t size;			/* returned/specified data length */

	uint32_t mem_size;		/* associated allocated memory size */

					/* callback return */
	int (*callback)(DB *, DBT *, DBT *);

	uint32_t flags;
};

/*******************************************
 * File handle
 *******************************************/
struct __db {
	ENV	*env;			/* Enclosing environment */
	BTREE	*btree;			/* Private object */

	void	*app_private;		/* Application-private information */

	TAILQ_ENTRY(__db) q;		/* List of handles in a session */

	uint32_t flags;

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * DB getter/setter variables: BEGIN
	 */
	int btree_compare_int;

	int (*btree_compare)(DB *, const DBT *, const DBT *);

	uint32_t intlitemsize;
	uint32_t leafitemsize;

	uint32_t allocsize;
	uint32_t intlmin;
	uint32_t intlmax;
	uint32_t leafmin;
	uint32_t leafmax;

	uint32_t fixed_len;
	const char *dictionary;

	void (*errcall)(const DB *, const char *);

	FILE *errfile;

	const char *errpfx;
	/*
	 * DB getter/setter variables: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * DB methods: BEGIN
	 */
	int (*btree_compare_get)(
	    DB *, int (**)(DB *, const DBT *, const DBT *));

	int (*btree_compare_int_get)(
	    DB *, int *);

	int (*btree_compare_int_set)(
	    DB *, int );

	int (*btree_compare_set)(
	    DB *, int (*)(DB *, const DBT *, const DBT *));

	int (*btree_itemsize_get)(
	    DB *, uint32_t *, uint32_t *);

	int (*btree_itemsize_set)(
	    DB *, uint32_t , uint32_t );

	int (*btree_pagesize_get)(
	    DB *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);

	int (*btree_pagesize_set)(
	    DB *, uint32_t , uint32_t , uint32_t , uint32_t , uint32_t );

	int (*bulk_load)(
	    DB *, void (*)(const char *, uint64_t), int (*)(DB *, DBT **, DBT **));

	int (*close)(
	    DB *, uint32_t );

	int (*col_del)(
	    DB *, WT_TOC *, uint64_t , uint32_t );

	int (*col_get)(
	    DB *, WT_TOC *, uint64_t , DBT *, uint32_t );

	int (*col_put)(
	    DB *, WT_TOC *, uint64_t , DBT *, uint32_t );

	int (*column_set)(
	    DB *, uint32_t , const char *, uint32_t );

	int (*dump)(
	    DB *, FILE *, void (*)(const char *, uint64_t), uint32_t );

	void (*err)(
	    DB *, int , const char *, ...);

	int (*errcall_get)(
	    DB *, void (**)(const DB *, const char *));

	int (*errcall_set)(
	    DB *, void (*)(const DB *, const char *));

	int (*errfile_get)(
	    DB *, FILE **);

	int (*errfile_set)(
	    DB *, FILE *);

	int (*errpfx_get)(
	    DB *, const char **);

	int (*errpfx_set)(
	    DB *, const char *);

	void (*errx)(
	    DB *, const char *, ...);

	int (*huffman_set)(
	    DB *, uint8_t const *, u_int , uint32_t );

	int (*open)(
	    DB *, const char *, mode_t , uint32_t );

	int (*row_del)(
	    DB *, WT_TOC *, DBT *, uint32_t );

	int (*row_get)(
	    DB *, WT_TOC *, DBT *, DBT *, uint32_t );

	int (*row_put)(
	    DB *, WT_TOC *, DBT *, DBT *, uint32_t );

	int (*stat_clear)(
	    DB *, uint32_t );

	int (*stat_print)(
	    DB *, FILE *, uint32_t );

	int (*sync)(
	    DB *, void (*)(const char *, uint64_t), uint32_t );

	int (*verify)(
	    DB *, void (*)(const char *, uint64_t), uint32_t );
	/*
	 * DB methods: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */
};

/*******************************************
 * Environment handle
 *******************************************/
struct __env {
	IENV	*ienv;			/* Private object */

	WT_ERROR_HANDLER *error_handler;

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * ENV getter/setter variables: BEGIN
	 */
	uint32_t cache_size;

	uint32_t data_update_initial;

	uint32_t data_update_max;

	void (*errcall)(const ENV *, const char *);

	FILE *errfile;

	const char *errpfx;

	uint32_t hazard_size;

	void (*msgcall)(const ENV *, const char *);

	FILE *msgfile;

	uint32_t toc_size;

	uint32_t verbose;
	/*
	 * ENV getter/setter variables: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * ENV methods: BEGIN
	 */
	int (*cache_size_get)(
	    ENV *, uint32_t *);

	int (*cache_size_set)(
	    ENV *, uint32_t );

	int (*close)(
	    ENV *, uint32_t );

	int (*data_update_initial_get)(
	    ENV *, uint32_t *);

	int (*data_update_initial_set)(
	    ENV *, uint32_t );

	int (*data_update_max_get)(
	    ENV *, uint32_t *);

	int (*data_update_max_set)(
	    ENV *, uint32_t );

	int (*db)(
	    ENV *, uint32_t , DB **);

	void (*err)(
	    ENV *, int , const char *, ...);

	int (*errcall_get)(
	    ENV *, void (**)(const ENV *, const char *));

	int (*errcall_set)(
	    ENV *, void (*)(const ENV *, const char *));

	int (*errfile_get)(
	    ENV *, FILE **);

	int (*errfile_set)(
	    ENV *, FILE *);

	int (*errpfx_get)(
	    ENV *, const char **);

	int (*errpfx_set)(
	    ENV *, const char *);

	void (*errx)(
	    ENV *, const char *, ...);

	int (*hazard_size_get)(
	    ENV *, uint32_t *);

	int (*hazard_size_set)(
	    ENV *, uint32_t );

	int (*msgcall_get)(
	    ENV *, void (**)(const ENV *, const char *));

	int (*msgcall_set)(
	    ENV *, void (*)(const ENV *, const char *));

	int (*msgfile_get)(
	    ENV *, FILE **);

	int (*msgfile_set)(
	    ENV *, FILE *);

	int (*open)(
	    ENV *, const char *, mode_t , uint32_t );

	int (*stat_clear)(
	    ENV *, uint32_t );

	int (*stat_print)(
	    ENV *, FILE *, uint32_t );

	int (*sync)(
	    ENV *, void (*)(const char *, uint64_t), uint32_t );

	int (*toc)(
	    ENV *, uint32_t , WT_TOC **);

	int (*toc_size_get)(
	    ENV *, uint32_t *);

	int (*toc_size_set)(
	    ENV *, uint32_t );

	int (*verbose_get)(
	    ENV *, uint32_t *);

	int (*verbose_set)(
	    ENV *, uint32_t );
	/*
	 * ENV methods: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	uint32_t flags;
};

/*******************************************
 * Prototypes.
 *******************************************/
int	 wiredtiger_env_init(ENV **, uint32_t);
void	 wiredtiger_err_stream(FILE *);
int	 wiredtiger_simple_setup(const char *, DB **, uint32_t, uint32_t);
int	 wiredtiger_simple_teardown(const char *, DB *);

/*******************************************
 * Application thread-of-control information
 *******************************************/
/*! @todo this has to become part of the WT_SESSION implementation. */
typedef	enum {
	WT_WORKQ_NONE=0,		/* No request */
	WT_WORKQ_FUNC=1,		/* Function, then return */
	WT_WORKQ_READ=2,		/* Function, then schedule read */
	WT_WORKQ_READ_SCHED=3		/* Waiting on read to complete */
} wq_state_t;

struct __wt_toc {
	WT_MTX	 *mtx;			/* Blocking mutex */

	const char *name;		/* Name */

	/*
	 * Enclosing environment, set when the WT_TOC is created (which
	 * implies that threads-of-control are confined to an environment).
	 *
	 * If NULL, the WT_TOC entry is not currently in use.
	 */
	ENV	*env;			/* Operation environment */

	void	*app_private;		/* Application-private information */

	DB	*db;			/* Current file */
	DBT	 key, data;		/* Returned key/data pairs */

	DBT	*scratch;		/* Temporary memory for any function */
	u_int	 scratch_alloc;		/* Currently allocated */

					/* WT_TOC workQ request */
	wq_state_t volatile wq_state;	/* Request state */
	int	  wq_ret;		/* Return value */
	int     (*wq_func)(WT_TOC *);	/* Function */
	void	 *wq_args;		/* Function argument */
	int	  wq_sleeping;		/* Thread is blocked */

	WT_PAGE	**hazard;		/* Hazard reference array */

	WT_FLIST *flist;		/* Memory free list */

	WT_TOC_BUFFER *tb;		/* Per-thread update buffer */

					/* Search return values: */
	WT_PAGE   *srch_page;		/*    page */
	uint32_t   srch_write_gen;	/*    page's write-generation */
	void	  *srch_ip;		/*    WT_{COL,ROW} index */
	WT_UPDATE *srch_upd;		/*    WT_UPD array index */
					/* RLE column-store only: */
	WT_RLE_EXPAND *srch_exp;	/*    WT_RLE_EXPAND array index */

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * WT_TOC methods: BEGIN
	 */
	int (*close)(
	    WT_TOC *, uint32_t );
	/*
	 * WT_TOC methods: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	 uint32_t flags;
};

/*******************************************
 * API flags
 *******************************************/
/*
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 * API flags section: BEGIN
 */
#define	WT_ASCII_ENGLISH				0x00000008
#define	WT_COLUMN					0x00000004
#define	WT_CREATE					0x00000001
#define	WT_DATA_OVERWRITE				0x00000001
#define	WT_DEBUG					0x00000002
#define	WT_HUFFMAN_DATA					0x00000004
#define	WT_HUFFMAN_KEY					0x00000002
#define	WT_INSERT					0x00000001
#define	WT_MEMORY_CHECK					0x00000001
#define	WT_NOWRITE					0x00000002
#define	WT_OSWRITE					0x00000001
#define	WT_PRINTABLES					0x00000001
#define	WT_RDONLY					0x00000002
#define	WT_READ_EVICT					0x00000002
#define	WT_READ_PRIORITY				0x00000001
#define	WT_RLE						0x00000001
#define	WT_SCRATCH_INUSE				0x00000001
#define	WT_SERVER_RUN					0x00000002
#define	WT_TELEPHONE					0x00000001
#define	WT_VERB_ALL					0x00000020
#define	WT_VERB_EVICT					0x00000010
#define	WT_VERB_FILEOPS					0x00000008
#define	WT_VERB_HAZARD					0x00000004
#define	WT_VERB_MUTEX					0x00000002
#define	WT_VERB_READ					0x00000001
#define	WT_WALK_CACHE					0x00000001
#define	WT_WORKQ_RUN					0x00000001

#define	WT_APIMASK_BT_SEARCH_COL			0x00000001
#define	WT_APIMASK_BT_SEARCH_KEY_ROW			0x00000001
#define	WT_APIMASK_BT_TREE_WALK				0x00000001
#define	WT_APIMASK_DB_CLOSE				0x00000003
#define	WT_APIMASK_DB_COL_DEL				0x00000000
#define	WT_APIMASK_DB_COL_GET				0x00000000
#define	WT_APIMASK_DB_COL_PUT				0x00000000
#define	WT_APIMASK_DB_COLUMN_SET			0x00000001
#define	WT_APIMASK_DB_DUMP				0x00000003
#define	WT_APIMASK_DB_HUFFMAN_SET			0x0000000f
#define	WT_APIMASK_DB_OPEN				0x00000003
#define	WT_APIMASK_DB_ROW_DEL				0x00000000
#define	WT_APIMASK_DB_ROW_GET				0x00000000
#define	WT_APIMASK_DB_ROW_PUT				0x00000000
#define	WT_APIMASK_DB_STAT_CLEAR			0x00000000
#define	WT_APIMASK_DB_STAT_PRINT			0x00000000
#define	WT_APIMASK_DB_SYNC				0x00000001
#define	WT_APIMASK_DB_VERIFY				0x00000000
#define	WT_APIMASK_DBT					0x00000001
#define	WT_APIMASK_ENV					0x00000001
#define	WT_APIMASK_ENV_CLOSE				0x00000000
#define	WT_APIMASK_ENV_DB				0x00000000
#define	WT_APIMASK_ENV_OPEN				0x00000000
#define	WT_APIMASK_ENV_STAT_CLEAR			0x00000000
#define	WT_APIMASK_ENV_STAT_PRINT			0x00000000
#define	WT_APIMASK_ENV_SYNC				0x00000000
#define	WT_APIMASK_ENV_TOC				0x00000000
#define	WT_APIMASK_ENV_VERBOSE_SET			0x0000003f
#define	WT_APIMASK_IDB					0x00000007
#define	WT_APIMASK_IENV					0x00000003
#define	WT_APIMASK_WIREDTIGER_ENV_INIT			0x00000001
#define	WT_APIMASK_WT_TOC				0x00000003
#define	WT_APIMASK_WT_TOC_CLOSE				0x00000000
/*
 * API flags section: END
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 */
