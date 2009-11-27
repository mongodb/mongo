b __wt_api_env_err
b __wt_api_env_errx
b __wt_assert
#b __wt_breakpoint
b __wt_database_format

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
