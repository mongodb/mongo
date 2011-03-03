/* DO NOT EDIT: automatically built by dist/serial.py. */

typedef struct {
	WT_PAGE * parent;
	void * parent_ref;
	int dsk_verify;
} __wt_cache_read_args;
#define	__wt_cache_read_serial(\
    session, _parent, _parent_ref, _dsk_verify, ret) do {\
	__wt_cache_read_args _args;\
	_args.parent = _parent;\
	_args.parent_ref = _parent_ref;\
	_args.dsk_verify = _dsk_verify;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_READ, 0, __wt_cache_read_serial_func, &_args);\
} while (0)
#define	__wt_cache_read_unpack(\
    session, _parent, _parent_ref, _dsk_verify) do {\
	_parent = ((__wt_cache_read_args *)(session)->wq_args)->parent;\
	_parent_ref = ((__wt_cache_read_args *)(session)->wq_args)->parent_ref;\
	_dsk_verify = ((__wt_cache_read_args *)(session)->wq_args)->dsk_verify;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	uint32_t slot;
	WT_UPDATE ** new_upd;
	WT_UPDATE * upd;
} __wt_item_update_args;
#define	__wt_item_update_serial(\
    session, _page, _write_gen, _slot, _new_upd, _upd, ret) do {\
	__wt_item_update_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.slot = _slot;\
	_args.new_upd = _new_upd;\
	_args.upd = _upd;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 1, __wt_item_update_serial_func, &_args);\
} while (0)
#define	__wt_item_update_unpack(\
    session, _page, _write_gen, _slot, _new_upd, _upd) do {\
	_page = ((__wt_item_update_args *)(session)->wq_args)->page;\
	_write_gen = ((__wt_item_update_args *)(session)->wq_args)->write_gen;\
	_slot = ((__wt_item_update_args *)(session)->wq_args)->slot;\
	_new_upd = ((__wt_item_update_args *)(session)->wq_args)->new_upd;\
	_upd = ((__wt_item_update_args *)(session)->wq_args)->upd;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	uint32_t slot;
	WT_RLE_EXPAND ** new_rleexp;
	WT_RLE_EXPAND * exp;
} __wt_rle_expand_args;
#define	__wt_rle_expand_serial(\
    session, _page, _write_gen, _slot, _new_rleexp, _exp, ret) do {\
	__wt_rle_expand_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.slot = _slot;\
	_args.new_rleexp = _new_rleexp;\
	_args.exp = _exp;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 1, __wt_rle_expand_serial_func, &_args);\
} while (0)
#define	__wt_rle_expand_unpack(\
    session, _page, _write_gen, _slot, _new_rleexp, _exp) do {\
	_page = ((__wt_rle_expand_args *)(session)->wq_args)->page;\
	_write_gen = ((__wt_rle_expand_args *)(session)->wq_args)->write_gen;\
	_slot = ((__wt_rle_expand_args *)(session)->wq_args)->slot;\
	_new_rleexp = ((__wt_rle_expand_args *)(session)->wq_args)->new_rleexp;\
	_exp = ((__wt_rle_expand_args *)(session)->wq_args)->exp;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	WT_RLE_EXPAND * exp;
	WT_UPDATE * upd;
} __wt_rle_expand_update_args;
#define	__wt_rle_expand_update_serial(\
    session, _page, _write_gen, _exp, _upd, ret) do {\
	__wt_rle_expand_update_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.exp = _exp;\
	_args.upd = _upd;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 1, __wt_rle_expand_update_serial_func, &_args);\
} while (0)
#define	__wt_rle_expand_update_unpack(\
    session, _page, _write_gen, _exp, _upd) do {\
	_page = ((__wt_rle_expand_update_args *)(session)->wq_args)->page;\
	_write_gen = ((__wt_rle_expand_update_args *)(session)->wq_args)->write_gen;\
	_exp = ((__wt_rle_expand_update_args *)(session)->wq_args)->exp;\
	_upd = ((__wt_rle_expand_update_args *)(session)->wq_args)->upd;\
} while (0)
