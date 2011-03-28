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
	WT_INSERT ** new_ins;
	WT_INSERT ** srch_ins;
	WT_INSERT * ins;
} __wt_insert_args;
#define	__wt_insert_serial(\
    session, _page, _write_gen, _new_ins, _srch_ins, _ins, ret) do {\
	__wt_insert_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.new_ins = _new_ins;\
	_args.srch_ins = _srch_ins;\
	_args.ins = _ins;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 1, __wt_insert_serial_func, &_args);\
} while (0)
#define	__wt_insert_unpack(\
    session, _page, _write_gen, _new_ins, _srch_ins, _ins) do {\
	__wt_insert_args *_args =\
	    (__wt_insert_args *)(session)->wq_args;\
	_page = _args->page;\
	_write_gen = _args->write_gen;\
	_new_ins = _args->new_ins;\
	_srch_ins = _args->srch_ins;\
	_ins = _args->ins;\
} while (0)

typedef struct {
	void * key_arg;
	WT_ITEM * item;
} __wt_key_build_args;
#define	__wt_key_build_serial(\
    session, _key_arg, _item, ret) do {\
	__wt_key_build_args _args;\
	_args.key_arg = _key_arg;\
	_args.item = _item;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 0, __wt_key_build_serial_func, &_args);\
} while (0)
#define	__wt_key_build_unpack(\
    session, _key_arg, _item) do {\
	__wt_key_build_args *_args =\
	    (__wt_key_build_args *)(session)->wq_args;\
	_key_arg = _args->key_arg;\
	_item = _args->item;\
} while (0)

typedef struct {
	WT_PAGE * page;
	uint32_t write_gen;
	WT_UPDATE ** new_upd;
	WT_UPDATE ** srch_upd;
	WT_UPDATE * upd;
} __wt_update_args;
#define	__wt_update_serial(\
    session, _page, _write_gen, _new_upd, _srch_upd, _upd, ret) do {\
	__wt_update_args _args;\
	_args.page = _page;\
	_args.write_gen = _write_gen;\
	_args.new_upd = _new_upd;\
	_args.srch_upd = _srch_upd;\
	_args.upd = _upd;\
	(ret) = __wt_session_serialize_func(session,\
	    WT_WORKQ_FUNC, 1, __wt_update_serial_func, &_args);\
} while (0)
#define	__wt_update_unpack(\
    session, _page, _write_gen, _new_upd, _srch_upd, _upd) do {\
	__wt_update_args *_args =\
	    (__wt_update_args *)(session)->wq_args;\
	_page = _args->page;\
	_write_gen = _args->write_gen;\
	_new_upd = _args->new_upd;\
	_srch_upd = _args->srch_upd;\
	_upd = _args->upd;\
} while (0)
