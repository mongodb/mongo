import os
import sys
from pathlib import Path
from pprint import pprint

import bson
import gdb

if not gdb:
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parent.parent.parent))
    from buildscripts.gdb.mongo import lookup_type

DEBUGGING = False
"""
Public API to be called by users. The input `ident` is a string of the form:
   'collection-2--4547167393143767234'.
 From within gdb type:
   python dump_pages_for_table('collection-2--4547167393143767234')

Some behaviors/limitations:
* Disk images of data are not deserialized into their separate key/value pairs.
* If update chain WT_UPDATEs are valid bson, the values will be parsed and output as BSON maps.
* If updates are not bson (e.g: index entries), they will be output as a raw byte array.
* WT_UPDATE structures have a pretty printer registered. Disabling pretty printers will result in
  more raw output.
* Any `file:*.wt` can be output, e.g: `_mdb_catalog` or `WiredTiger`. Though the output may be less
  supported/of lower quality.
"""


def dump_pages_for_table(ident):
    conn_impl_type = lookup_type("WT_CONNECTION_IMPL")
    if not conn_impl_type:
        print(
            "WT_CONNECTION_IMPL type not found. Try invoking this function from a different \
thread and frame."
        )
        return

    conn_impl_ptr_type = conn_impl_type.pointer()
    dbg("impl", conn_impl_ptr_type)

    conn_ptr = None
    try:
        conn_ptr = gdb.parse_and_eval("session->iface->connection")
    except gdb.error:
        pass

    if not conn_ptr or not conn_ptr.address:
        print(
            "Failed to find a suitable `WT_SESSION session` object to extract a connection object \
from. Try finding an eviction thread and frame, e.g: `__wt_evict_thread_run`. If the session is \
optimized out, try going up stack frames until the variable is in a local scope rather than a \
function input."
        )
        return

    conn = conn_ptr.reinterpret_cast(conn_impl_ptr_type).dereference()
    dbg("conn", conn)
    data_handle, all_dhs = get_data_handle(conn, "file:{}.wt".format(ident))
    if not data_handle:
        print("Data handle not found for ident. Ident: `{}`".format(ident))
        print("All known data handles:")
        pprint(all_dhs)
        return

    dump_handle(data_handle)


# Private API.
def dbg(ident, var):
    if not DEBUGGING:
        return

    print("----------")
    if type(var) == gdb.Value:
        print("{}: ({}*){}".format(ident, var.type, var.address))
    else:
        print(ident)
    print("  " + str(type(var)))
    methods = dir(var)
    out = [name for name in methods if not name.startswith("__")]
    for item in out:
        print("    " + item)

    if type(var) == gdb.Value:
        print("\n  Fields:")
        print("\t" + "\n\t".join(str(var).split("\n")))


def walk_wt_list(lst):
    ret = []
    node = lst["tqh_first"]
    dbg("node", node)
    while True:
        if not node:
            break
        ret.append(node.dereference())
        node = node["q"]["tqe_next"]

    return ret


def get_data_handle(conn, handle_name):
    dbg("datahandles", conn["dhqh"])
    ret = None
    all_file_dhs = []
    for handle in walk_wt_list(conn["dhqh"]):
        if handle["name"].string().startswith("file:"):
            all_file_dhs.append(handle["name"].string()[5:-3])
        if handle["name"].string() == handle_name:
            ret = handle

    return ret, all_file_dhs


def get_btree_handle(dhandle):
    btree = lookup_type("WT_BTREE").pointer()
    return dhandle["handle"].reinterpret_cast(btree).dereference()


def dump_update_chain(update_chain):
    while True:
        if not update_chain:
            print("  λ (End of update chain)")
            break
        dbg("update", update_chain)
        wt_val = update_chain.dereference()
        obj = None
        dbg("wt_val", wt_val)
        val_bytes = gdb.selected_inferior().read_memory(wt_val["data"], wt_val["size"])
        can_bson = wt_val["type"] == 3
        if can_bson:
            try:
                obj = bson.decode_all(val_bytes)[0]
            except:
                pass
        print("  " + "\n  ".join(str(wt_val).split("\n")) + " " + str(obj) + " =>")

        update_chain = update_chain["next"]


def dump_insert_list(wt_insert):
    key_struct = wt_insert["u"]["key"]
    key = (
        gdb.selected_inferior()
        .read_memory(int(wt_insert.address) + key_struct["offset"], key_struct["size"])
        .tobytes()
    )
    print("Key: " + str(key))
    print("Value:")
    update_chain = wt_insert["upd"]
    dump_update_chain(update_chain)


def dump_skip_list(wt_insert_head):
    if not wt_insert_head["head"].address:
        return
    wt_insert = wt_insert_head["head"][0]
    idx = 0
    while True:
        if not wt_insert:
            break
        dump_insert_list(wt_insert.dereference())
        dbg("insert" + str(idx), wt_insert.dereference())
        idx += 1
        wt_insert = wt_insert["next"][0]


def dump_modified(leaf_page):
    print("Modify:")
    if not leaf_page["modify"]:
        print("No modifies")
        return

    leaf_modify = leaf_page["modify"].dereference()
    dbg("modify", leaf_modify)
    row_leaf_insert = leaf_modify["u2"]["row_leaf"]["insert"]
    dbg("row store", row_leaf_insert)
    if not row_leaf_insert:
        print("No insert list")
    else:
        print("Insert list:")
        dump_skip_list(row_leaf_insert.dereference().dereference())

    row_leaf_update = leaf_modify["u2"]["row_leaf"]["update"]
    if not row_leaf_update:
        print("No update list")
    else:
        print("Update list:")
        leaf_num_entries = int(leaf_page["entries"])
        for i in range(0, leaf_num_entries):
            dump_update_chain(row_leaf_update[i])


def dump_disk(leaf_page):
    dbg("in-memory page:", leaf_page)
    dsk = leaf_page["dsk"].dereference()
    if int(dsk.address) == 0:
        print("No page loaded from disk.")
        return
    dbg("on-disk page:", dsk)
    wt_page_header_size = 28
    wt_block_header_size = 12
    page_bytes = (
        gdb.selected_inferior()
        .read_memory(
            int(dsk.address) + wt_page_header_size + wt_block_header_size, int(dsk["mem_size"])
        )
        .tobytes()
    )
    print("Dsk:\n" + str(page_bytes))


def dump_handle(dhandle):
    print("Dumping: " + dhandle["name"].string())
    btree = get_btree_handle(dhandle)
    root = btree["root"]
    root_page = root["page"].dereference()
    dbg("btree", btree)
    dbg("root", btree["root"])
    dbg("root page", root_page)
    rpindex = root_page["u"]["intl"]["__index"].dereference()
    leaf_num_entries = int(rpindex["entries"])
    for idx in range(0, leaf_num_entries):
        dbg("rpindex", rpindex)
        dbg("rp-pre-index", rpindex["index"].dereference().dereference())
        leaf_page = rpindex["index"][idx].dereference()["page"].dereference()
        dbg("leaf", leaf_page)
        dump_disk(leaf_page)
        dump_modified(leaf_page)
