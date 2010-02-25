/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_ROW_INDX * indx;
	WT_REPL * repl;
} __wt_bt_del_args;
#define	 __wt_bt_del_serial(toc, _indx, _repl) do {\
	__wt_bt_del_args _args;\
	_args.indx = _indx;\
	_args.repl = _repl;\
	__wt_toc_serialize_request(\
	    toc, __wt_bt_del_serial_func, &_args);\
} while (0)
#define	__wt_bt_del_unpack(toc, _indx, _repl) do {\
	_indx =\
	    ((__wt_bt_del_args *)(toc)->serial_args)->indx;\
	_repl =\
	    ((__wt_bt_del_args *)(toc)->serial_args)->repl;\
} while (0)

typedef struct {
	WT_BIN_INDX * bp;
	WT_BIN_INDX * new;
	int isleft;
} __wt_bt_insert_args;
#define	 __wt_bt_insert_serial(toc, _bp, _new, _isleft) do {\
	__wt_bt_insert_args _args;\
	_args.bp = _bp;\
	_args.new = _new;\
	_args.isleft = _isleft;\
	__wt_toc_serialize_request(\
	    toc, __wt_bt_insert_serial_func, &_args);\
} while (0)
#define	__wt_bt_insert_unpack(toc, _bp, _new, _isleft) do {\
	_bp =\
	    ((__wt_bt_insert_args *)(toc)->serial_args)->bp;\
	_new =\
	    ((__wt_bt_insert_args *)(toc)->serial_args)->new;\
	_isleft =\
	    ((__wt_bt_insert_args *)(toc)->serial_args)->isleft;\
} while (0)

typedef struct {
	WT_ROW_INDX * indx;
	WT_REPL * repl;
	void * data;
	u_int32_t size;
} __wt_bt_repl_args;
#define	 __wt_bt_repl_serial(toc, _indx, _repl, _data, _size) do {\
	__wt_bt_repl_args _args;\
	_args.indx = _indx;\
	_args.repl = _repl;\
	_args.data = _data;\
	_args.size = _size;\
	__wt_toc_serialize_request(\
	    toc, __wt_bt_repl_serial_func, &_args);\
} while (0)
#define	__wt_bt_repl_unpack(toc, _indx, _repl, _data, _size) do {\
	_indx =\
	    ((__wt_bt_repl_args *)(toc)->serial_args)->indx;\
	_repl =\
	    ((__wt_bt_repl_args *)(toc)->serial_args)->repl;\
	_data =\
	    ((__wt_bt_repl_args *)(toc)->serial_args)->data;\
	_size =\
	    ((__wt_bt_repl_args *)(toc)->serial_args)->size;\
} while (0)

typedef struct {
	WT_REF * drain;
	u_int32_t drain_elem;
} __wt_cache_discard_args;
#define	 __wt_cache_discard_serial(toc, _drain, _drain_elem) do {\
	__wt_cache_discard_args _args;\
	_args.drain = _drain;\
	_args.drain_elem = _drain_elem;\
	__wt_toc_serialize_request(\
	    toc, __wt_cache_discard_serial_func, &_args);\
} while (0)
#define	__wt_cache_discard_unpack(toc, _drain, _drain_elem) do {\
	_drain =\
	    ((__wt_cache_discard_args *)(toc)->serial_args)->drain;\
	_drain_elem =\
	    ((__wt_cache_discard_args *)(toc)->serial_args)->drain_elem;\
} while (0)

typedef struct {
	WT_PAGE * page;
	u_int32_t addr;
	u_int32_t bytes;
} __wt_cache_in_args;
#define	 __wt_cache_in_serial(toc, _page, _addr, _bytes) do {\
	__wt_cache_in_args _args;\
	_args.page = _page;\
	_args.addr = _addr;\
	_args.bytes = _bytes;\
	__wt_toc_serialize_request(\
	    toc, __wt_cache_in_serial_func, &_args);\
} while (0)
#define	__wt_cache_in_unpack(toc, _page, _addr, _bytes) do {\
	_page =\
	    ((__wt_cache_in_args *)(toc)->serial_args)->page;\
	_addr =\
	    ((__wt_cache_in_args *)(toc)->serial_args)->addr;\
	_bytes =\
	    ((__wt_cache_in_args *)(toc)->serial_args)->bytes;\
} while (0)
