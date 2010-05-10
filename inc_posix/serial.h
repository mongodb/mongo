/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE *page;
	WT_REPL *new;
} __wt_bt_del_args;
#define	 __wt_bt_del_serial(toc, _page, _new, ret) do {\
	__wt_bt_del_args _args;\
	_args.page = _page;\
	_args.new = _new;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_bt_del_serial_func, &_args);\
} while (0)
#define	__wt_bt_del_unpack(toc, _page, _new) do {\
	_page = ((__wt_bt_del_args *)(toc)->wq_args)->page;\
	_new = ((__wt_bt_del_args *)(toc)->wq_args)->new;\
} while (0)

typedef struct {
	WT_ROW_INDX *indx;
	WT_REPL *repl;
	void *data;
	u_int32_t size;
} __wt_bt_repl_args;
#define	 __wt_bt_repl_serial(toc, _indx, _repl, _data, _size, ret) do {\
	__wt_bt_repl_args _args;\
	_args.indx = _indx;\
	_args.repl = _repl;\
	_args.data = _data;\
	_args.size = _size;\
	(ret) = __wt_toc_serialize_func(\
	    toc, WT_WORKQ_SPIN, __wt_bt_repl_serial_func, &_args);\
} while (0)
#define	__wt_bt_repl_unpack(toc, _indx, _repl, _data, _size) do {\
	_indx = ((__wt_bt_repl_args *)(toc)->wq_args)->indx;\
	_repl = ((__wt_bt_repl_args *)(toc)->wq_args)->repl;\
	_data = ((__wt_bt_repl_args *)(toc)->wq_args)->data;\
	_size = ((__wt_bt_repl_args *)(toc)->wq_args)->size;\
} while (0)

typedef struct {
	u_int32_t addr;
	u_int32_t size;
	WT_PAGE **pagep;
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
	WT_FLIST *flistp;
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
