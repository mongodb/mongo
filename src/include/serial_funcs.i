/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE *page;
	WT_INSERT_HEAD **inshead;
	WT_INSERT ***ins_stack;
	WT_INSERT_HEAD **new_inslist;
	size_t new_inslist_size;
	int new_inslist_taken;
	WT_INSERT_HEAD *new_inshead;
	size_t new_inshead_size;
	int new_inshead_taken;
	WT_INSERT *new_ins;
	size_t new_ins_size;
	int new_ins_taken;
	u_int skipdepth;
} __wt_col_append_args;

static inline int
__wt_col_append_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, WT_INSERT_HEAD **inshead,
	WT_INSERT ***ins_stack, WT_INSERT_HEAD ***new_inslistp, size_t
	new_inslist_size, WT_INSERT_HEAD **new_insheadp, size_t
	new_inshead_size, WT_INSERT **new_insp, size_t new_ins_size, u_int
	skipdepth)
{
	__wt_col_append_args _args, *args = &_args;
	WT_DECL_RET;

	args->page = page;

	args->inshead = inshead;

	args->ins_stack = ins_stack;

	if (new_inslistp == NULL)
		args->new_inslist = NULL;
	else {
		args->new_inslist = *new_inslistp;
		*new_inslistp = NULL;
		args->new_inslist_size = new_inslist_size;
	}
	args->new_inslist_taken = 0;

	if (new_insheadp == NULL)
		args->new_inshead = NULL;
	else {
		args->new_inshead = *new_insheadp;
		*new_insheadp = NULL;
		args->new_inshead_size = new_inshead_size;
	}
	args->new_inshead_taken = 0;

	if (new_insp == NULL)
		args->new_ins = NULL;
	else {
		args->new_ins = *new_insp;
		*new_insp = NULL;
		args->new_ins_size = new_ins_size;
	}
	args->new_ins_taken = 0;

	args->skipdepth = skipdepth;

	ret = __wt_session_serialize_func(session,
	    WT_SERIAL_FUNC, __wt_col_append_serial_func, args);

	if (!args->new_inslist_taken)
		__wt_free(session, args->new_inslist);
	if (!args->new_inshead_taken)
		__wt_free(session, args->new_inshead);
	if (!args->new_ins_taken)
		__wt_free(session, args->new_ins);
	return (ret);
}

static inline void
__wt_col_append_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, WT_INSERT_HEAD ***insheadp,
	WT_INSERT ****ins_stackp, WT_INSERT_HEAD ***new_inslistp,
	WT_INSERT_HEAD **new_insheadp, WT_INSERT **new_insp, u_int
	*skipdepthp)
{
	__wt_col_append_args *args =
	    (__wt_col_append_args *)session->wq_args;

	*pagep = args->page;
	*insheadp = args->inshead;
	*ins_stackp = args->ins_stack;
	*new_inslistp = args->new_inslist;
	*new_insheadp = args->new_inshead;
	*new_insp = args->new_ins;
	*skipdepthp = args->skipdepth;
}

static inline void
__wt_col_append_new_inslist_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_append_args *args =
	    (__wt_col_append_args *)session->wq_args;

	args->new_inslist_taken = 1;

	WT_ASSERT(session, args->new_inslist_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_inslist_size);
}

static inline void
__wt_col_append_new_inshead_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_append_args *args =
	    (__wt_col_append_args *)session->wq_args;

	args->new_inshead_taken = 1;

	WT_ASSERT(session, args->new_inshead_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_inshead_size);
}

static inline void
__wt_col_append_new_ins_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_col_append_args *args =
	    (__wt_col_append_args *)session->wq_args;

	args->new_ins_taken = 1;

	WT_ASSERT(session, args->new_ins_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_ins_size);
}

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_INSERT_HEAD **inshead;
	WT_INSERT ***ins_stack;
	WT_INSERT_HEAD **new_inslist;
	size_t new_inslist_size;
	int new_inslist_taken;
	WT_INSERT_HEAD *new_inshead;
	size_t new_inshead_size;
	int new_inshead_taken;
	WT_INSERT *new_ins;
	size_t new_ins_size;
	int new_ins_taken;
	u_int skipdepth;
} __wt_insert_args;

static inline int
__wt_insert_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen,
	WT_INSERT_HEAD **inshead, WT_INSERT ***ins_stack, WT_INSERT_HEAD
	***new_inslistp, size_t new_inslist_size, WT_INSERT_HEAD
	**new_insheadp, size_t new_inshead_size, WT_INSERT **new_insp, size_t
	new_ins_size, u_int skipdepth)
{
	__wt_insert_args _args, *args = &_args;
	WT_DECL_RET;

	args->page = page;

	args->write_gen = write_gen;

	args->inshead = inshead;

	args->ins_stack = ins_stack;

	if (new_inslistp == NULL)
		args->new_inslist = NULL;
	else {
		args->new_inslist = *new_inslistp;
		*new_inslistp = NULL;
		args->new_inslist_size = new_inslist_size;
	}
	args->new_inslist_taken = 0;

	if (new_insheadp == NULL)
		args->new_inshead = NULL;
	else {
		args->new_inshead = *new_insheadp;
		*new_insheadp = NULL;
		args->new_inshead_size = new_inshead_size;
	}
	args->new_inshead_taken = 0;

	if (new_insp == NULL)
		args->new_ins = NULL;
	else {
		args->new_ins = *new_insp;
		*new_insp = NULL;
		args->new_ins_size = new_ins_size;
	}
	args->new_ins_taken = 0;

	args->skipdepth = skipdepth;

	ret = __wt_session_serialize_func(session,
	    WT_SERIAL_FUNC, __wt_insert_serial_func, args);

	if (!args->new_inslist_taken)
		__wt_free(session, args->new_inslist);
	if (!args->new_inshead_taken)
		__wt_free(session, args->new_inshead);
	if (!args->new_ins_taken)
		__wt_free(session, args->new_ins);
	return (ret);
}

static inline void
__wt_insert_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, uint32_t *write_genp,
	WT_INSERT_HEAD ***insheadp, WT_INSERT ****ins_stackp, WT_INSERT_HEAD
	***new_inslistp, WT_INSERT_HEAD **new_insheadp, WT_INSERT **new_insp,
	u_int *skipdepthp)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*insheadp = args->inshead;
	*ins_stackp = args->ins_stack;
	*new_inslistp = args->new_inslist;
	*new_insheadp = args->new_inshead;
	*new_insp = args->new_ins;
	*skipdepthp = args->skipdepth;
}

static inline void
__wt_insert_new_inslist_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	args->new_inslist_taken = 1;

	WT_ASSERT(session, args->new_inslist_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_inslist_size);
}

static inline void
__wt_insert_new_inshead_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	args->new_inshead_taken = 1;

	WT_ASSERT(session, args->new_inshead_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_inshead_size);
}

static inline void
__wt_insert_new_ins_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_insert_args *args =
	    (__wt_insert_args *)session->wq_args;

	args->new_ins_taken = 1;

	WT_ASSERT(session, args->new_ins_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_ins_size);
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
	WT_DECL_RET;

	args->page = page;

	args->row_arg = row_arg;

	args->ikey = ikey;

	ret = __wt_session_serialize_func(session,
	    WT_SERIAL_FUNC, __wt_row_key_serial_func, args);

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
	int syncop;
} __wt_sync_file_args;

static inline int
__wt_sync_file_serial(
	WT_SESSION_IMPL *session, int syncop)
{
	__wt_sync_file_args _args, *args = &_args;
	WT_DECL_RET;

	args->syncop = syncop;

	ret = __wt_session_serialize_func(session,
	    WT_SERIAL_EVICT, __wt_sync_file_serial_func, args);

	return (ret);
}

static inline void
__wt_sync_file_unpack(
	WT_SESSION_IMPL *session, int *syncopp)
{
	__wt_sync_file_args *args =
	    (__wt_sync_file_args *)session->wq_args;

	*syncopp = args->syncop;
}

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_UPDATE **srch_upd;
	WT_UPDATE **new_upd;
	size_t new_upd_size;
	int new_upd_taken;
	WT_UPDATE *upd;
	size_t upd_size;
	int upd_taken;
} __wt_update_args;

static inline int
__wt_update_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen, WT_UPDATE
	**srch_upd, WT_UPDATE ***new_updp, size_t new_upd_size, WT_UPDATE
	**updp, size_t upd_size)
{
	__wt_update_args _args, *args = &_args;
	WT_DECL_RET;

	args->page = page;

	args->write_gen = write_gen;

	args->srch_upd = srch_upd;

	if (new_updp == NULL)
		args->new_upd = NULL;
	else {
		args->new_upd = *new_updp;
		*new_updp = NULL;
		args->new_upd_size = new_upd_size;
	}
	args->new_upd_taken = 0;

	if (updp == NULL)
		args->upd = NULL;
	else {
		args->upd = *updp;
		*updp = NULL;
		args->upd_size = upd_size;
	}
	args->upd_taken = 0;

	ret = __wt_session_serialize_func(session,
	    WT_SERIAL_FUNC, __wt_update_serial_func, args);

	if (!args->new_upd_taken)
		__wt_free(session, args->new_upd);
	if (!args->upd_taken)
		__wt_free(session, args->upd);
	return (ret);
}

static inline void
__wt_update_unpack(
	WT_SESSION_IMPL *session, WT_PAGE **pagep, uint32_t *write_genp,
	WT_UPDATE ***srch_updp, WT_UPDATE ***new_updp, WT_UPDATE **updp)
{
	__wt_update_args *args =
	    (__wt_update_args *)session->wq_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*srch_updp = args->srch_upd;
	*new_updp = args->new_upd;
	*updp = args->upd;
}

static inline void
__wt_update_new_upd_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_update_args *args =
	    (__wt_update_args *)session->wq_args;

	args->new_upd_taken = 1;

	WT_ASSERT(session, args->new_upd_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->new_upd_size);
}

static inline void
__wt_update_upd_taken(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_update_args *args =
	    (__wt_update_args *)session->wq_args;

	args->upd_taken = 1;

	WT_ASSERT(session, args->upd_size != 0);
	__wt_cache_page_inmem_incr(session, page, args->upd_size);
}
