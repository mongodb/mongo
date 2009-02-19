b __wt_assert
b __wt_database_format

b __wt_debug_loadme

define da
print __wt_bt_dump_addr(db, $arg0, "DUMP", 0)
end
define di
print __wt_bt_dump_ipage(db, $arg0, "DUMP", 0)
end
define dp
print __wt_bt_dump_page(db, $arg0, "DUMP", 0)
end
define dd
print __wt_bt_dump(db, "DUMP", 0)
end
