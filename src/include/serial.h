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
	__wt_cache_read_args *_args =\
	    (__wt_cache_read_args *)(session)->wq_args;\
	_parent = _args->parent;\
	_parent_ref = _args->parent_ref;\
	_dsk_verify = _args->dsk_verify;\
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
	__wt_item_update_args *_args =\
	    (__wt_item_update_args *)(session)->wq_args;\
	_page = _args->page;\
	_write_gen = _args->write_gen;\
	_slot = _args->slot;\
	_new_upd = _args->new_upd;\
	_upd = _args->upd;\
} while (0)

typedef struct {
	void * key_arg;
	WT_BUF * tmp;
} __wt_key_build_args;
#define	__wt_key_build_serial(\
    session, _key_arg, _tmp, ret) do {\
	__wt_key_build_args _args;\
	_args.key_arg = _key_arg;\
	_args.tmp = _tmp;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 0, __wt_key_build_serial_func, &_args);\
} while (0)
#define	__wt_key_build_unpack(\
    session, _key_arg, _tmp) do {\
	__wt_key_build_args *_args =\
	    (__wt_key_build_args *)(session)->wq_args;\
	_key_arg = _args->key_arg;\
	_tmp = _args->tmp;\
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
	__wt_rle_expand_args *_args =\
	    (__wt_rle_expand_args *)(session)->wq_args;\
	_page = _args->page;\
	_write_gen = _args->write_gen;\
	_slot = _args->slot;\
	_new_rleexp = _args->new_rleexp;\
	_exp = _args->exp;\
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
	__wt_rle_expand_update_args *_args =\
	    (__wt_rle_expand_update_args *)(session)->wq_args;\
	_page = _args->page;\
	_write_gen = _args->write_gen;\
	_exp = _args->exp;\
	_upd = _args->upd;\
} while (0)
