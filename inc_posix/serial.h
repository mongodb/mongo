/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE * page;
	int slot;
	WT_COL_EXPAND ** new_expcol;
	WT_COL_EXPAND * exp;
} __wt_bt_rcc_expand_args;
#define	 __wt_bt_rcc_expand_serial(toc, _page, _slot, _new_expcol, _exp, ret) do {\
	__wt_bt_rcc_expand_args _args;\
	_args.page = _page;\
	_args.slot = _slot;\
	_args.new_expcol = _new_expcol;\
	_args.exp = _exp;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_bt_rcc_expand_serial_func, &_args);\
} while (0)
#define	__wt_bt_rcc_expand_unpack(toc, _page, _slot, _new_expcol, _exp) do {\
	_page = ((__wt_bt_rcc_expand_args *)(toc)->wq_args)->page;\
	_slot = ((__wt_bt_rcc_expand_args *)(toc)->wq_args)->slot;\
	_new_expcol = ((__wt_bt_rcc_expand_args *)(toc)->wq_args)->new_expcol;\
	_exp = ((__wt_bt_rcc_expand_args *)(toc)->wq_args)->exp;\
} while (0)

typedef struct {
	WT_PAGE * page;
	WT_COL_EXPAND * exp;
	WT_REPL * repl;
} __wt_bt_rcc_expand_repl_args;
#define	 __wt_bt_rcc_expand_repl_serial(toc, _page, _exp, _repl, ret) do {\
	__wt_bt_rcc_expand_repl_args _args;\
	_args.page = _page;\
	_args.exp = _exp;\
	_args.repl = _repl;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_bt_rcc_expand_repl_serial_func, &_args);\
} while (0)
#define	__wt_bt_rcc_expand_repl_unpack(toc, _page, _exp, _repl) do {\
	_page = ((__wt_bt_rcc_expand_repl_args *)(toc)->wq_args)->page;\
	_exp = ((__wt_bt_rcc_expand_repl_args *)(toc)->wq_args)->exp;\
	_repl = ((__wt_bt_rcc_expand_repl_args *)(toc)->wq_args)->repl;\
} while (0)

typedef struct {
	WT_ROW * indx;
	void * data;
	u_int32_t size;
} __wt_bt_replace_args;
#define	 __wt_bt_replace_serial(toc, _indx, _data, _size, ret) do {\
	__wt_bt_replace_args _args;\
	_args.indx = _indx;\
	_args.data = _data;\
	_args.size = _size;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_bt_replace_serial_func, &_args);\
} while (0)
#define	__wt_bt_replace_unpack(toc, _indx, _data, _size) do {\
	_indx = ((__wt_bt_replace_args *)(toc)->wq_args)->indx;\
	_data = ((__wt_bt_replace_args *)(toc)->wq_args)->data;\
	_size = ((__wt_bt_replace_args *)(toc)->wq_args)->size;\
} while (0)

typedef struct {
	WT_PAGE * page;
	int slot;
	WT_REPL ** new_repl;
	WT_REPL * repl;
} __wt_bt_update_args;
#define	 __wt_bt_update_serial(toc, _page, _slot, _new_repl, _repl, ret) do {\
	__wt_bt_update_args _args;\
	_args.page = _page;\
	_args.slot = _slot;\
	_args.new_repl = _new_repl;\
	_args.repl = _repl;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_bt_update_serial_func, &_args);\
} while (0)
#define	__wt_bt_update_unpack(toc, _page, _slot, _new_repl, _repl) do {\
	_page = ((__wt_bt_update_args *)(toc)->wq_args)->page;\
	_slot = ((__wt_bt_update_args *)(toc)->wq_args)->slot;\
	_new_repl = ((__wt_bt_update_args *)(toc)->wq_args)->new_repl;\
	_repl = ((__wt_bt_update_args *)(toc)->wq_args)->repl;\
} while (0)

typedef struct {
	u_int32_t addr;
	u_int32_t size;
	WT_PAGE ** pagep;
} __wt_cache_in_args;
#define	 __wt_cache_in_serial(toc, _addr, _size, _pagep, ret) do {\
	__wt_cache_in_args _args;\
	_args.addr = _addr;\
	_args.size = _size;\
	_args.pagep = _pagep;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_READ, __wt_cache_in_serial_func, &_args);\
} while (0)
#define	__wt_cache_in_unpack(toc, _addr, _size, _pagep) do {\
	_addr = ((__wt_cache_in_args *)(toc)->wq_args)->addr;\
	_size = ((__wt_cache_in_args *)(toc)->wq_args)->size;\
	_pagep = ((__wt_cache_in_args *)(toc)->wq_args)->pagep;\
} while (0)

typedef struct {
	WT_FLIST * flistp;
} __wt_flist_free_args;
#define	 __wt_flist_free_serial(toc, _flistp, ret) do {\
	__wt_flist_free_args _args;\
	_args.flistp = _flistp;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_flist_free_serial_func, &_args);\
} while (0)
#define	__wt_flist_free_unpack(toc, _flistp) do {\
	_flistp = ((__wt_flist_free_args *)(toc)->wq_args)->flistp;\
} while (0)
