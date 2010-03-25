/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_ROW_INDX * indx;
	WT_REPL * repl;
} __wt_bt_del_args;
#define	 __wt_bt_del_serial(toc, _indx, _repl, ret) do {\
	__wt_bt_del_args _args;\
	_args.indx = _indx;\
	_args.repl = _repl;\
	(ret) = __wt_toc_serialize_func(\
	    toc, __wt_bt_del_serial_func, &_args);\
} while (0)
#define	__wt_bt_del_unpack(toc, _indx, _repl) do {\
	_indx =\
	    ((__wt_bt_del_args *)(toc)->wq_args)->indx;\
	_repl =\
	    ((__wt_bt_del_args *)(toc)->wq_args)->repl;\
} while (0)

typedef struct {
	WT_BIN_INDX * bp;
	WT_BIN_INDX * new;
	int isleft;
} __wt_bt_insert_args;
#define	 __wt_bt_insert_serial(toc, _bp, _new, _isleft, ret) do {\
	__wt_bt_insert_args _args;\
	_args.bp = _bp;\
	_args.new = _new;\
	_args.isleft = _isleft;\
	(ret) = __wt_toc_serialize_func(\
	    toc, __wt_bt_insert_serial_func, &_args);\
} while (0)
#define	__wt_bt_insert_unpack(toc, _bp, _new, _isleft) do {\
	_bp =\
	    ((__wt_bt_insert_args *)(toc)->wq_args)->bp;\
	_new =\
	    ((__wt_bt_insert_args *)(toc)->wq_args)->new;\
	_isleft =\
	    ((__wt_bt_insert_args *)(toc)->wq_args)->isleft;\
} while (0)

typedef struct {
	WT_ROW_INDX * indx;
	WT_REPL * repl;
	void * data;
	u_int32_t size;
} __wt_bt_repl_args;
#define	 __wt_bt_repl_serial(toc, _indx, _repl, _data, _size, ret) do {\
	__wt_bt_repl_args _args;\
	_args.indx = _indx;\
	_args.repl = _repl;\
	_args.data = _data;\
	_args.size = _size;\
	(ret) = __wt_toc_serialize_func(\
	    toc, __wt_bt_repl_serial_func, &_args);\
} while (0)
#define	__wt_bt_repl_unpack(toc, _indx, _repl, _data, _size) do {\
	_indx =\
	    ((__wt_bt_repl_args *)(toc)->wq_args)->indx;\
	_repl =\
	    ((__wt_bt_repl_args *)(toc)->wq_args)->repl;\
	_data =\
	    ((__wt_bt_repl_args *)(toc)->wq_args)->data;\
	_size =\
	    ((__wt_bt_repl_args *)(toc)->wq_args)->size;\
} while (0)

typedef struct {
	WT_FLIST * fp;
} __wt_flist_free_args;
#define	 __wt_flist_free_serial(toc, _fp, ret) do {\
	__wt_flist_free_args _args;\
	_args.fp = _fp;\
	(ret) = __wt_toc_serialize_func(\
	    toc, __wt_flist_free_serial_func, &_args);\
} while (0)
#define	__wt_flist_free_unpack(toc, _fp) do {\
	_fp =\
	    ((__wt_flist_free_args *)(toc)->wq_args)->fp;\
} while (0)
