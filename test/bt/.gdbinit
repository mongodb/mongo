b __wt_api_env_err
b __wt_api_env_errx
b __wt_api_db_err
b __wt_api_db_errx
b __wt_assert
b __wt_assert
#b __wt_breakpoint
b __wt_database_format

define pd
print __wt_debug_disk(toc, $arg0, "/tmp/o", 0)
end
define pp
print __wt_debug_page(toc, $arg0, "/tmp/o", 0)
end
define pi
print __wt_debug_item(toc, $arg0, 0)
end
define dumpfile
print __wt_debug_dump(toc, "/tmp/o", 0)
end
