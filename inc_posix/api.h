/* DO NOT EDIT: automatically built by dist/api.py. */

/*
 * Do not clear the DB handle in the WT_TOC schedule macro, we may be doing a
 * WT_TOC call from within a DB call.
 */
#define	wt_api_toc_sched(oparg)\
	toc->op = (oparg);\
	toc->argp = &args;\
	return (__wt_toc_sched(wt_toc))
#define	wt_api_db_sched(oparg)\
	toc->op = (oparg);\
	toc->db = db;\
	toc->argp = &args;\
	return (__wt_toc_sched(wt_toc))
