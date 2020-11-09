# Create entries used by our doxygen filter to expand the arch_page
# macros in the documentation.

class ArchDocPage:
    def __init__(self, doxygen_name, data_structures, files):
        self.doxygen_name = doxygen_name
        self.data_structures = data_structures
        self.files = files

##########################################
# List of all architecture subsections
##########################################
arch_doc_pages = [
    ArchDocPage('arch-dhandle',
        ['WT_BTREE', 'WT_DHANDLE'],
        ['src/include/btree.h', 'src/include/dhandle.h',
         'src/conn/conn_dhandle.c', 'src/session/session_dhandle.c']),
    ArchDocPage('arch-schema',
        ['WT_COLGROUP', 'WT_INDEX', 'WT_LSM_TREE', 'WT_TABLE'],
        ['src/include/intpack_inline.h', 'src/include/packing_inline.h',
         'src/include/schema.h',
         'src/lsm/', 'src/packing/', 'src/schema/']),
    ArchDocPage('arch-transaction',
        ['WT_TXN', 'WT_TXN_GLOBAL', 'WT_TXN_OP', 'WT_TXN_SHARED'],
        ['src/include/txn.h', 'src/include/txn_inline.h', 'src/txn/']),
]
