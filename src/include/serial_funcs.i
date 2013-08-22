/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_INSERT_HEAD **insheadp;
	WT_INSERT ***ins_stack;
	WT_INSERT **next_stack;
	WT_INSERT_HEAD **new_inslist;
	int new_inslist_taken;
	WT_INSERT_HEAD *new_inshead;
	int new_inshead_taken;
	WT_INSERT *new_ins;
	int new_ins_taken;
	u_int skipdepth;
} __wt_col_append_args;

static inline int
__wt_col_append_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen,
	WT_INSERT_HEAD **insheadp, WT_INSERT ***ins_stack, WT_INSERT
	**next_stack, WT_INSERT_HEAD ***new_inslistp, size_t new_inslist_size,
	WT_INSERT_HEAD **new_insheadp, size_t new_inshead_size, WT_INSERT
	**new_insp, size_t new_ins_size, u_int skipdepth) {
	__wt_col_append_args _args, *args = &_args;
	WT_DECL_RET;
	size_t incr_mem;

	args->page = page;

	args->write_gen = write_gen;

	args->insheadp = insheadp;

	args->ins_stack = ins_stack;

	args->next_stack = next_stack;

	if (new_inslistp == NULL)
		args->new_inslist = NULL;
	else {
		args->new_inslist = *new_inslistp;
		*new_inslistp = NULL;
	}
	args->new_inslist_taken = 0;

	if (new_insheadp == NULL)
		args->new_inshead = NULL;
	else {
		args->new_inshead = *new_insheadp;
		*new_insheadp = NULL;
	}
	args->new_inshead_taken = 0;

	if (new_insp == NULL)
		args->new_ins = NULL;
	else {
		args->new_ins = *new_insp;
		*new_insp = NULL;
	}
	args->new_ins_taken = 0;

	args->skipdepth = skipdepth;

	__wt_spin_lock(session, &S2C(session)->serial_lock);
	ret = __wt_col_append_serial_func(session, args);

	/* Increment in-memory footprint before decrement is possible. */
	incr_mem = 0;
	if (args->new_inslist_taken) {
		WT_ASSERT(session, new_inslist_size != 0);
		incr_mem += new_inslist_size;
	}
	if (args->new_inshead_taken) {
		WT_ASSERT(session, new_inshead_size != 0);
		incr_mem += new_inshead_size;
	}
	if (args->new_ins_taken) {
		WT_ASSERT(session, new_ins_size != 0);
		incr_mem += new_ins_size;
	}
	if (incr_mem != 0)
		__wt_cache_page_inmem_incr(session, page, incr_mem);

	__wt_spin_unlock(session, &S2C(session)->serial_lock);

	/* Free any unused memory after releasing serialization mutex. */
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
    void *untyped_args, WT_PAGE **pagep, uint32_t *write_genp,
    WT_INSERT_HEAD ***insheadpp, WT_INSERT ****ins_stackp, WT_INSERT
    ***next_stackp, WT_INSERT_HEAD ***new_inslistp, WT_INSERT_HEAD
    **new_insheadp, WT_INSERT **new_insp, u_int *skipdepthp)
{
	__wt_col_append_args *args = (__wt_col_append_args *)untyped_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*insheadpp = args->insheadp;
	*ins_stackp = args->ins_stack;
	*next_stackp = args->next_stack;
	*new_inslistp = args->new_inslist;
	*new_insheadp = args->new_inshead;
	*new_insp = args->new_ins;
	*skipdepthp = args->skipdepth;
}

static inline void
__wt_col_append_new_inslist_taken(void *untyped_args)
{
	__wt_col_append_args *args = (__wt_col_append_args *)untyped_args;

	args->new_inslist_taken = 1;
}

static inline void
__wt_col_append_new_inshead_taken(void *untyped_args)
{
	__wt_col_append_args *args = (__wt_col_append_args *)untyped_args;

	args->new_inshead_taken = 1;
}

static inline void
__wt_col_append_new_ins_taken(void *untyped_args)
{
	__wt_col_append_args *args = (__wt_col_append_args *)untyped_args;

	args->new_ins_taken = 1;
}

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_INSERT_HEAD **inshead;
	WT_INSERT ***ins_stack;
	WT_INSERT **next_stack;
	WT_INSERT_HEAD **new_inslist;
	int new_inslist_taken;
	WT_INSERT_HEAD *new_inshead;
	int new_inshead_taken;
	WT_INSERT *new_ins;
	int new_ins_taken;
	u_int skipdepth;
} __wt_insert_args;

static inline int
__wt_insert_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen,
	WT_INSERT_HEAD **inshead, WT_INSERT ***ins_stack, WT_INSERT
	**next_stack, WT_INSERT_HEAD ***new_inslistp, size_t new_inslist_size,
	WT_INSERT_HEAD **new_insheadp, size_t new_inshead_size, WT_INSERT
	**new_insp, size_t new_ins_size, u_int skipdepth) {
	__wt_insert_args _args, *args = &_args;
	WT_DECL_RET;
	size_t incr_mem;

	args->page = page;

	args->write_gen = write_gen;

	args->inshead = inshead;

	args->ins_stack = ins_stack;

	args->next_stack = next_stack;

	if (new_inslistp == NULL)
		args->new_inslist = NULL;
	else {
		args->new_inslist = *new_inslistp;
		*new_inslistp = NULL;
	}
	args->new_inslist_taken = 0;

	if (new_insheadp == NULL)
		args->new_inshead = NULL;
	else {
		args->new_inshead = *new_insheadp;
		*new_insheadp = NULL;
	}
	args->new_inshead_taken = 0;

	if (new_insp == NULL)
		args->new_ins = NULL;
	else {
		args->new_ins = *new_insp;
		*new_insp = NULL;
	}
	args->new_ins_taken = 0;

	args->skipdepth = skipdepth;

	__wt_spin_lock(session, &S2C(session)->serial_lock);
	ret = __wt_insert_serial_func(session, args);

	/* Increment in-memory footprint before decrement is possible. */
	incr_mem = 0;
	if (args->new_inslist_taken) {
		WT_ASSERT(session, new_inslist_size != 0);
		incr_mem += new_inslist_size;
	}
	if (args->new_inshead_taken) {
		WT_ASSERT(session, new_inshead_size != 0);
		incr_mem += new_inshead_size;
	}
	if (args->new_ins_taken) {
		WT_ASSERT(session, new_ins_size != 0);
		incr_mem += new_ins_size;
	}
	if (incr_mem != 0)
		__wt_cache_page_inmem_incr(session, page, incr_mem);

	__wt_spin_unlock(session, &S2C(session)->serial_lock);

	/* Free any unused memory after releasing serialization mutex. */
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
    void *untyped_args, WT_PAGE **pagep, uint32_t *write_genp,
    WT_INSERT_HEAD ***insheadp, WT_INSERT ****ins_stackp, WT_INSERT
    ***next_stackp, WT_INSERT_HEAD ***new_inslistp, WT_INSERT_HEAD
    **new_insheadp, WT_INSERT **new_insp, u_int *skipdepthp)
{
	__wt_insert_args *args = (__wt_insert_args *)untyped_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*insheadp = args->inshead;
	*ins_stackp = args->ins_stack;
	*next_stackp = args->next_stack;
	*new_inslistp = args->new_inslist;
	*new_insheadp = args->new_inshead;
	*new_insp = args->new_ins;
	*skipdepthp = args->skipdepth;
}

static inline void
__wt_insert_new_inslist_taken(void *untyped_args)
{
	__wt_insert_args *args = (__wt_insert_args *)untyped_args;

	args->new_inslist_taken = 1;
}

static inline void
__wt_insert_new_inshead_taken(void *untyped_args)
{
	__wt_insert_args *args = (__wt_insert_args *)untyped_args;

	args->new_inshead_taken = 1;
}

static inline void
__wt_insert_new_ins_taken(void *untyped_args)
{
	__wt_insert_args *args = (__wt_insert_args *)untyped_args;

	args->new_ins_taken = 1;
}

typedef struct {
	WT_PAGE *page;
	uint32_t write_gen;
	WT_UPDATE **srch_upd;
	WT_UPDATE *old_upd;
	WT_UPDATE *upd;
	int upd_taken;
	WT_UPDATE **upd_obsolete;
} __wt_update_args;

static inline int
__wt_update_serial(
	WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen, WT_UPDATE
	**srch_upd, WT_UPDATE *old_upd, WT_UPDATE **updp, size_t upd_size,
	WT_UPDATE **upd_obsolete) {
	__wt_update_args _args, *args = &_args;
	WT_DECL_RET;
	size_t incr_mem;

	args->page = page;

	args->write_gen = write_gen;

	args->srch_upd = srch_upd;

	args->old_upd = old_upd;

	if (updp == NULL)
		args->upd = NULL;
	else {
		args->upd = *updp;
		*updp = NULL;
	}
	args->upd_taken = 0;

	args->upd_obsolete = upd_obsolete;

	__wt_spin_lock(session, &S2C(session)->serial_lock);
	ret = __wt_update_serial_func(session, args);

	/* Increment in-memory footprint before decrement is possible. */
	incr_mem = 0;
	if (args->upd_taken) {
		WT_ASSERT(session, upd_size != 0);
		incr_mem += upd_size;
	}
	if (incr_mem != 0)
		__wt_cache_page_inmem_incr(session, page, incr_mem);

	__wt_spin_unlock(session, &S2C(session)->serial_lock);

	/* Free any unused memory after releasing serialization mutex. */
	if (!args->upd_taken)
		__wt_free(session, args->upd);

	return (ret);
}

static inline void
__wt_update_unpack(
    void *untyped_args, WT_PAGE **pagep, uint32_t *write_genp, WT_UPDATE
    ***srch_updp, WT_UPDATE **old_updp, WT_UPDATE **updp, WT_UPDATE
    ***upd_obsoletep)
{
	__wt_update_args *args = (__wt_update_args *)untyped_args;

	*pagep = args->page;
	*write_genp = args->write_gen;
	*srch_updp = args->srch_upd;
	*old_updp = args->old_upd;
	*updp = args->upd;
	*upd_obsoletep = args->upd_obsolete;
}

static inline void
__wt_update_upd_taken(void *untyped_args)
{
	__wt_update_args *args = (__wt_update_args *)untyped_args;

	args->upd_taken = 1;
}
