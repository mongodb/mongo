import gdb
import bson
from pprint import pprint

# FIXME-WT-9286 Update to use gdb.Command like the other python scripts.

# Usage examples:
# To dump data from the original file handle.
# coll_dh = get_data_handle(conn, 'file:collection-5550--7194480883124807592.wt', NULL)
# dump_handle(coll_dh)
#
# To dump data from the checkpoint of file handle.
# coll_dh = get_data_handle(conn, 'file:collection-5550--7194480883124807592.wt', 'WiredTigerCheckpoint.5')
# dump_handle(coll_dh)


def dbg(ident, var):
    print('----------')
    if type(var) == gdb.Value:
        print('{}: ({}*){}'.format(ident, var.type, var.address))
    else:
        print(ident)
    print('  ' + str(type(var)))
    methods = dir(var)
    out = [name for name in methods if not name.startswith("__")]
    for item in out:
        print('    ' + item)

    if type(var) == gdb.Value:
        print('\n  Fields:')
        print('\t' + '\n\t'.join(str(var).split('\n')))

conn_impl_ptr = gdb.lookup_type("WT_CONNECTION_IMPL").pointer()
dbg('impl', conn_impl_ptr)

conn = gdb.parse_and_eval("session->iface->connection").reinterpret_cast(conn_impl_ptr).dereference()
dbg('conn', conn)

def walk_wt_list(lst):
    ret = []
    node = conn['dhqh']['tqh_first']
    #dbg('node', node)
    while True:
        if not node:
            break
        ret.append(node.dereference())
        node = node['q']['tqe_next']

    return ret

def get_data_handle(conn, handle_name, checkpoint_name=None):
    #dbg('datahandles', conn['dhqh'])
    ret = None
    for handle in walk_wt_list(conn['dhqh']):
        #print("Handle: " + str(handle['name']))
        #print("Handle checkpoint: " + str(handle['checkpoint']))
        if handle['name'].string() == handle_name:
            if checkpoint_name and handle['checkpoint'] and handle['checkpoint'].string() == checkpoint_name:
                ret = handle
            elif not checkpoint_name:
                ret = handle
    if ret:
        print("Found Handle: " + str(ret['name']))
        print("Found Handle checkpoint: " + str(ret['checkpoint']))
    return ret

def get_btree_handle(dhandle):
    btree = gdb.lookup_type('WT_BTREE').pointer()
    return dhandle['handle'].reinterpret_cast(btree).dereference()

def dump_update_chain(update_chain):
    while True:
        if not update_chain:
            print('  λ')
            break
        #dbg('update', update_chain)
        wt_val = update_chain.dereference()
        obj = None
        #dbg('wt_val', wt_val)
        val_bytes = gdb.selected_inferior().read_memory(wt_val['data'], wt_val['size'])
        can_bson = wt_val['type'] == 3
        if can_bson:
            try:
                obj = bson.decode_all(val_bytes)[0]
                if obj["_id"]['id'] and obj["_id"]["id"].subtype == 4:
                    obj["_id"]["id"] = obj["_id"]["id"].as_uuid()
            except:
                pass
        print('  ' + '\n  '.join(str(wt_val).split('\n')) + " " + str(obj) + " =>")

        update_chain = update_chain['next']

def dump_insert_list(wt_insert):
    key_struct = wt_insert['u']['key']
    key = gdb.selected_inferior().read_memory(int(wt_insert.address) + key_struct['offset'], key_struct['size']).tobytes()
    print('Key: ' + str(key))
    print('Value:')
    update_chain = wt_insert['upd']
    dump_update_chain(update_chain)

def dump_skip_list(wt_insert_head):
    wt_insert = wt_insert_head['head'][0]
    if wt_insert.address == 0:
        print("  Null insert list.")
        return
    idx = 0
    while True:
        if not wt_insert:
            break
        dump_insert_list(wt_insert.dereference())
        #dbg('insert' + str(idx), wt_insert.dereference())
        idx+=1
        wt_insert = wt_insert['next'][0]

def dump_modified(leaf_page):
    print("Modify:")
    if not leaf_page['modify']:
        print("No modifies")
        return

    leaf_modify = leaf_page['modify'].dereference()
    #dbg('modify', leaf_modify)
    row_leaf_insert = leaf_modify['u2']['row_leaf']['insert']
    #dbg('row store', row_leaf_modify)
    if not row_leaf_insert:
        print("No insert list")
    else:
        print("Insert list:")
        dump_skip_list(row_leaf_insert.dereference().dereference())
    
    row_leaf_update = leaf_modify['u2']['row_leaf']['update']
    if not row_leaf_update:
        print("No update list")
    else:
        print("Update list (({}){}):".format(leaf_page.type.name, leaf_page.address))
        leaf_num_entries = int(leaf_page['entries'])
        for i in range(0, leaf_num_entries):
            dump_update_chain(row_leaf_update[i])

def dump_disk(leaf_page):
    leaf_num_entries = int(leaf_page['entries'])
    dbg('in-memory page:', leaf_page)
    dsk = leaf_page['dsk'].dereference()
    if int(dsk.address) == 0:
        print("No page loaded from disk.")
        return
    dbg('on-disk page:', dsk)
    wt_page_header_size = 28 # defined as WT_PAGE_HEADER_SIZE in btmem.h
    wt_block_header_size = 12 # defined as WT_BLOCK_HEADER_SIZE in block.h
    page_bytes = gdb.selected_inferior().read_memory(int(dsk.address) + wt_page_header_size + wt_block_header_size, int(dsk['mem_size'])).tobytes()
    print("Dsk:\n" + str(page_bytes))

def dump_leaf_page(leaf_page):
    dbg('leaf', leaf_page)
    dump_disk(leaf_page)
    dump_modified(leaf_page)

def dump_int_page(int_page):
    dbg('Internal page', int_page)
    pindex = int_page['u']['intl']['__index'].dereference()
    dbg('pindex', pindex)
    num_entries = int(pindex['entries'])
    for i in range(0, num_entries):
        if not pindex['index'][i].dereference()['page']:
            continue
        page = pindex['index'][i].dereference()['page'].dereference()
        dbg('page', page)
        page_type = page['type']
        dbg('page type', page_type)
        # The page types WT_PAGE_COL_INT and WT_PAGE_ROW_INT are set for
        # internal pages of the tree. These values are defined in btmem.h
        if page_type == 3 or page_type == 6:
            dump_int_page(page)
        else:
            dump_leaf_page(page)

def dump_handle(dhandle):
    print("Dumping: " + dhandle['name'].string())
    btree = get_btree_handle(dhandle)
    root = btree['root']
    root_page = root['page'].dereference()
    #dbg('btree', get_btree_handle(user))
    dbg('root', btree['root'])
    dbg('root page', root_page)
    dump_int_page(root_page)

