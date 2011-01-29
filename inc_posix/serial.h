/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE * parent;
	WT_REF * ref;
	WT_OFF * off;
	int dsk_verify;
} __wt_cache_read_args;
#define	__wt_cache_read_serial(\
    toc, _parent, _ref, _off, _dsk_verify, ret) do {\
	__wt_cache_read_args _args;\
	_args.parent = _parent;\
	_args.ref = _ref;\
	_args.off = _off;\
	_args.dsk_verify = _dsk_verify;\
	(ret) = __wt_toc_serialize_func(toc,\
	    WT_WORKQ_READ, 0, __wt_cache_read_serial_func, &_args);\
} while (0)
#define	__wt_cache_read_unpack(\
    toc, _parent, _ref, _off, _dsk_verify) do {\
	_parent = ((__wt_cache_read_args *)(toc)->wq_args)->parent;\
	_ref = ((__wt_cache_read_args *)(toc)->wq_args)->ref;\
	_off = ((__wt_cache_read_args *)(toc)->wq_args)->off;\
	_dsk_verify = ((__wt_cache_read_args *)(toc)->wq_args)->dsk_verify;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	uint32_t slot;
	WT_REPL ** new_repl;
	WT_REPL * repl;
} __wt_item_update_args;
#define	__wt_item_update_serial(\
    toc, _page, _write_gen, _slot, _new_repl, _repl, ret) do {\
	__wt_item_update_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.slot = _slot;\
	_args.new_repl = _new_repl;\
	_args.repl = _repl;\
	(ret) = __wt_toc_serialize_func(toc,\
	    WT_WORKQ_FUNC, 1, __wt_item_update_serial_func, &_args);\
} while (0)
#define	__wt_item_update_unpack(\
    toc, _page, _write_gen, _slot, _new_repl, _repl) do {\
	_page = ((__wt_item_update_args *)(toc)->wq_args)->page;\
	_write_gen = ((__wt_item_update_args *)(toc)->wq_args)->write_gen;\
	_slot = ((__wt_item_update_args *)(toc)->wq_args)->slot;\
	_new_repl = ((__wt_item_update_args *)(toc)->wq_args)->new_repl;\
	_repl = ((__wt_item_update_args *)(toc)->wq_args)->repl;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	uint32_t slot;
	WT_RCC_EXPAND ** new_rccexp;
	WT_RCC_EXPAND * exp;
} __wt_rcc_expand_args;
#define	__wt_rcc_expand_serial(\
    toc, _page, _write_gen, _slot, _new_rccexp, _exp, ret) do {\
	__wt_rcc_expand_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.slot = _slot;\
	_args.new_rccexp = _new_rccexp;\
	_args.exp = _exp;\
	(ret) = __wt_toc_serialize_func(toc,\
	    WT_WORKQ_FUNC, 1, __wt_rcc_expand_serial_func, &_args);\
} while (0)
#define	__wt_rcc_expand_unpack(\
    toc, _page, _write_gen, _slot, _new_rccexp, _exp) do {\
	_page = ((__wt_rcc_expand_args *)(toc)->wq_args)->page;\
	_write_gen = ((__wt_rcc_expand_args *)(toc)->wq_args)->write_gen;\
	_slot = ((__wt_rcc_expand_args *)(toc)->wq_args)->slot;\
	_new_rccexp = ((__wt_rcc_expand_args *)(toc)->wq_args)->new_rccexp;\
	_exp = ((__wt_rcc_expand_args *)(toc)->wq_args)->exp;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	WT_RCC_EXPAND * exp;
	WT_REPL * repl;
} __wt_rcc_expand_repl_args;
#define	__wt_rcc_expand_repl_serial(\
    toc, _page, _write_gen, _exp, _repl, ret) do {\
	__wt_rcc_expand_repl_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.exp = _exp;\
	_args.repl = _repl;\
	(ret) = __wt_toc_serialize_func(toc,\
	    WT_WORKQ_FUNC, 1, __wt_rcc_expand_repl_serial_func, &_args);\
} while (0)
#define	__wt_rcc_expand_repl_unpack(\
    toc, _page, _write_gen, _exp, _repl) do {\
	_page = ((__wt_rcc_expand_repl_args *)(toc)->wq_args)->page;\
	_write_gen = ((__wt_rcc_expand_repl_args *)(toc)->wq_args)->write_gen;\
	_exp = ((__wt_rcc_expand_repl_args *)(toc)->wq_args)->exp;\
	_repl = ((__wt_rcc_expand_repl_args *)(toc)->wq_args)->repl;\
} while (0)
