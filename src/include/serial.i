/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE *parent;
	WT_REF *parent_ref;
	int dsk_verify;
} __wt_cache_read_args;

static inline int
__wt_cache_read_serial(
	WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *parent_ref, int
	dsk_verify)
{
	__wt_cache_read_args _args, *args = &_args;
	int ret;

	args->parent = parent;

	args->parent_ref = parent_ref;

	args->dsk_verify = dsk_verify;

	ret = __wt_session_serialize_func(session,
	    WT_WORKQ_READ, 0, __wt_cache_read_serial_func, args);

	return (ret);
}

static inline void
__wt_cache_read_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **parentp, WT_REF **parent_refp, int
	*dsk_verifyp)
{
	__wt_cache_read_args *args =
	    (__wt_cache_read_args *)session->wq_args;

	*parentp = args->parent;
	*parent_refp = args->parent_ref;
	*dsk_verifyp = args->dsk_verify;
}

typedef struct {
	WT_PAGE *page;
	WT_PAGE *new_intl;
	uint32_t new_intl_size;
	int new_intl_taken;
	WT_COL_REF *t;
	uint32_t t_size;
	int t_taken;
	uint32_t internal_extend;
	WT_PAGE *new_leaf;
	uint32_t new_leaf_size;
	int new_leaf_taken;
	void *entries;
	uint32_t entries_size;
	int entries_taken;
	uint32_t leaf_extend;
	uint64_t recno;
} __wt_col_extend_args;

static inline int
__wt_col_extend_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE **new_intlp, uint32_t
	new_intl_size, WT_COL_REF **tp, uint32_t t_size, uint32_t
	internal_extend, WT_PAGE **new_leafp, uint32_t new_leaf_size, void
	**entriesp, uint32_t entries_size, uint32_t leaf_extend, uint64_t
	recno)
{
	__wt_col_extend_args _args, *args = &_args;
	int ret;

	args->page = page;

	if (new_intlp == NULL)
		args->new_intl = NULL;
	else {
		args->new_intl = *new_intlp;
		*new_intlp = NULL;
		args->new_intl_size = new_intl_size;
	}
	args->new_intl_taken = 0;

	if (tp == NULL)
		args->t = NULL;
	else {
		args->t = *tp;
		*tp = NULL;
		args->t_size = t_size;
	}
	args->t_taken = 0;

	args->internal_extend = internal_extend;

	if (new_leafp == NULL)
		args->new_leaf = NULL;
	else {
		args->new_leaf = *new_leafp;
		*new_leafp = NULL;
		args->new_leaf_size = new_leaf_size;
	}
	args->new_leaf_taken = 0;

	if (entriesp == NULL)
		args->entries = NULL;
	else {
		args->entries = *entriesp;
		*entriesp = NULL;
		args->entries_size = entries_size;
	}
	args->entries_taken = 0;

	args->leaf_extend = leaf_extend;

	args->recno = recno;

	ret = __wt_session_serialize_func(session,
	    WT_WORKQ_FUNC, 1, __wt_col_extend_serial_func, args);

	if (!args->new_intl_taken)
		__wt_free(session, args->new_intl);
	if (!args->t_taken)
		__wt_free(session, args->t);
	if (!args->new_leaf_taken)
		__wt_free(session, args->new_leaf);
	if (!args->entries_taken)
		__wt_free(session, args->entries);
	return (ret);
}

static inline void
__wt_col_extend_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, WT_PAGE **new_intlp,
	WT_COL_REF **tp, uint32_t *internal_extendp, WT_PAGE **new_leafp, void
	**entriesp, uint32_t *leaf_extendp, uint64_t *recnop)
{
	__wt_col_extend_args *args =
	    (__wt_col_extend_args *)session->wq_args;

	*pagep = args->page;
	*new_intlp = args->new_intl;
	*tp = args->t;
	*internal_extendp = args->internal_extend;
	*new_leafp = args->new_leaf;
	*entriesp = args->entries;
	*leaf_extendp = args->leaf_extend;
	*recnop = args->recno;
}

static inline void
__wt_col_extend_new_intl_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_extend_args *args =
	    (__wt_col_extend_args *)session->wq_args;

	args->new_intl_taken = 1;

	WT_ASSERT(session, args->new_intl_size != 0);
	__wt_cache_page_workq_incr(session, page, args->new_intl_size);
}

static inline void
__wt_col_extend_t_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_extend_args *args =
	    (__wt_col_extend_args *)session->wq_args;

	args->t_taken = 1;

	WT_ASSERT(session, args->t_size != 0);
	__wt_cache_page_workq_incr(session, page, args->t_size);
}

static inline void
__wt_col_extend_new_leaf_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_extend_args *args =
	    (__wt_col_extend_args *)session->wq_args;

	args->new_leaf_taken = 1;

	WT_ASSERT(session, args->new_leaf_size != 0);
	__wt_cache_page_workq_incr(session, page, args->new_leaf_size);
}

static inline void
__wt_col_extend_entries_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_extend_args *args =
	    (__wt_col_extend_args *)session->wq_args;

	args->entries_taken = 1;

	WT_ASSERT(session, args->entries_size != 0);
	__wt_cache_page_workq_incr(session, page, args->entries_size);
}

typedef struct {
	int close_method;
} __wt_evict_file_args;

static inline int
__wt_evict_file_serial(
	WT_SESSION_IMPL *session, int close_method)
{
	__wt_evict_file_args _args, *args = &_args;
	int ret;

	args->close_method = close_method;

	ret = __wt_session_serialize_func(session,
	    WT_WORKQ_EVICT, 0, __wt_evict_file_serial_func, args);

	return (ret);
}

static inline void
__wt_evict_file_unpack(
	WT_SESSION_IMPL *session, int *close_methodp)
{
	__wt_evict_file_args *args =
	    (__wt_evict_file_args *)session->wq_args;

	*close_methodp = args->close_method;
}

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_INSERT **new_ins;
	uint32_t new_ins_size;
	int new_ins_taken;
	WT_INSERT **srch_ins;
	WT_INSERT *ins;
	uint32_t ins_size;
	int ins_taken;
} __wt_insert_args;

static inline int
__wt_insert_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen, WT_INSERT
	***new_insp, uint32_t new_ins_size, WT_INSERT **srch_ins, WT_INSERT
	**insp, uint32_t ins_size)
{
	__wt_insert_args _args, *args = &_args;
	int ret;

	args->page = page;

	args->write_gen = write_gen;

	if (new_insp == NULL)
		args->new_ins = NULL;
	else {
		args->new_ins = *new_insp;
		*new_insp = NULL;
		args->new_ins_size = new_ins_size;
	}
	args->new_ins_taken = 0;

	args->srch_ins = srch_ins;

	if (insp == NULL)
		args->ins = NULL;
	else {
		args->ins = *insp;
		*insp = NULL;
		args->ins_size = ins_size;
	}
	args->ins_taken = 0;

	ret = __wt_session_serialize_func(session,
	    WT_WORKQ_FUNC, 1, __wt_insert_serial_func, args);

	if (!args->new_ins_taken)
		__wt_free(session, args->new_ins);
	if (!args->ins_taken)
		__wt_free(session, args->ins);
	return (ret);
}

static inline void
__wt_insert_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, uint32_t *write_genp,
	WT_INSERT ***new_insp, WT_INSERT ***srch_insp, WT_INSERT **insp)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*new_insp = args->new_ins;
	*srch_insp = args->srch_ins;
	*insp = args->ins;
}

static inline void
__wt_insert_new_ins_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	args->new_ins_taken = 1;

	WT_ASSERT(session, args->new_ins_size != 0);
	__wt_cache_page_workq_incr(session, page, args->new_ins_size);
}

static inline void
__wt_insert_ins_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	args->ins_taken = 1;

	WT_ASSERT(session, args->ins_size != 0);
	__wt_cache_page_workq_incr(session, page, args->ins_size);
}

typedef struct {
	WT_PAGE *page;
	WT_ROW *row_arg;
	WT_IKEY *ikey;
} __wt_row_key_args;

static inline int
__wt_row_key_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *row_arg, WT_IKEY
	*ikey)
{
	__wt_row_key_args _args, *args = &_args;
	int ret;

	args->page = page;

	args->row_arg = row_arg;

	args->ikey = ikey;

	ret = __wt_session_serialize_func(session,
	    WT_WORKQ_FUNC, 1, __wt_row_key_serial_func, args);

	return (ret);
}

static inline void
__wt_row_key_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, WT_ROW **row_argp, WT_IKEY
	**ikeyp)
{
	__wt_row_key_args *args =
	    (__wt_row_key_args *)session->wq_args;

	*pagep = args->page;
	*row_argp = args->row_arg;
	*ikeyp = args->ikey;
}

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_UPDATE **new_upd;
	uint32_t new_upd_size;
	int new_upd_taken;
	WT_UPDATE **srch_upd;
	WT_UPDATE *upd;
	uint32_t upd_size;
	int upd_taken;
} __wt_update_args;

static inline int
__wt_update_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen, WT_UPDATE
	***new_updp, uint32_t new_upd_size, WT_UPDATE **srch_upd, WT_UPDATE
	**updp, uint32_t upd_size)
{
	__wt_update_args _args, *args = &_args;
	int ret;

	args->page = page;

	args->write_gen = write_gen;

	if (new_updp == NULL)
		args->new_upd = NULL;
	else {
		args->new_upd = *new_updp;
		*new_updp = NULL;
		args->new_upd_size = new_upd_size;
	}
	args->new_upd_taken = 0;

	args->srch_upd = srch_upd;

	if (updp == NULL)
		args->upd = NULL;
	else {
		args->upd = *updp;
		*updp = NULL;
		args->upd_size = upd_size;
	}
	args->upd_taken = 0;

	ret = __wt_session_serialize_func(session,
	    WT_WORKQ_FUNC, 1, __wt_update_serial_func, args);

	if (!args->new_upd_taken)
		__wt_free(session, args->new_upd);
	if (!args->upd_taken)
		__wt_free(session, args->upd);
	return (ret);
}

static inline void
__wt_update_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, uint32_t *write_genp,
	WT_UPDATE ***new_updp, WT_UPDATE ***srch_updp, WT_UPDATE **updp)
{
	__wt_update_args *args =
	    (__wt_update_args *)session->wq_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*new_updp = args->new_upd;
	*srch_updp = args->srch_upd;
	*updp = args->upd;
}

static inline void
__wt_update_new_upd_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_update_args *args =
	    (__wt_update_args *)session->wq_args;

	args->new_upd_taken = 1;

	WT_ASSERT(session, args->new_upd_size != 0);
	__wt_cache_page_workq_incr(session, page, args->new_upd_size);
}

static inline void
__wt_update_upd_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_update_args *args =
	    (__wt_update_args *)session->wq_args;

	args->upd_taken = 1;

	WT_ASSERT(session, args->upd_size != 0);
	__wt_cache_page_workq_incr(session, page, args->upd_size);
}
