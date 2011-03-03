b __wt_errv
b __wt_assert
#b __wt_breakpoint
b __wt_file_format

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
