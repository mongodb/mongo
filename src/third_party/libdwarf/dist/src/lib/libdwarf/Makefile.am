###Copyright (C) 2018 Vincent Torri <vincent dot torri at gmail dot com
###This code is public domain and can be freely used or copied.

AUTOMAKE_OPTIONS=subdir-objects
MAINTAINERCLEANFILES = Makefile.in

### libdwarf

lib_LTLIBRARIES = libdwarf.la

libdwarf_la_SOURCES = \
dwarf.h \
dwarf_abbrev.c \
dwarf_abbrev.h \
dwarf_alloc.c \
dwarf_alloc.h \
dwarf_arange.c \
dwarf_arange.h \
dwarf_base_types.h \
dwarf_crc.c \
dwarf_crc32.c \
dwarf_debugaddr.c \
dwarf_debugaddr.h \
dwarf_debuglink.c \
dwarf_debuglink.h \
dwarf_die_deliv.c \
dwarf_die_deliv.h \
dwarf_debugnames.c \
dwarf_debugnames.h \
dwarf_debug_sup.c \
dwarf_dsc.c \
dwarf_dsc.h \
dwarf_elf_access.h \
dwarf_elf_defines.h \
dwarf_elf_load_headers.c \
dwarf_elfread.c \
dwarf_elfread.h \
dwarf_elf_rel_detector.c \
dwarf_elf_rel_detector.h \
dwarf_elfstructs.h \
dwarf_errmsg_list.h \
dwarf_error.c \
dwarf_error.h \
dwarf_fill_in_attr_form.c \
dwarf_find_sigref.c \
dwarf_fission_to_cu.c \
dwarf_form.c \
dwarf_form_class_names.c \
dwarf_frame.c \
dwarf_frame.h \
dwarf_frame2.c \
dwarf_gdbindex.c \
dwarf_gdbindex.h \
dwarf_generic_init.c \
dwarf_global.c \
dwarf_global.h \
dwarf_gnu_index.h \
dwarf_gnu_index.c \
dwarf_groups.c \
dwarf_harmless.c \
dwarf_harmless.h \
dwarf_init_finish.c \
dwarf_leb.c \
dwarf_line.c \
dwarf_line.h \
dwarf_line_table_reader_common.h \
dwarf_loc.c \
dwarf_loc.h \
dwarf_loclists.h \
dwarf_locationop_read.c \
dwarf_loclists.c \
dwarf_macho_loader.h \
dwarf_machoread.c \
dwarf_machoread.h \
dwarf_macro.c \
dwarf_macro.h \
dwarf_macro5.c \
dwarf_macro5.h \
dwarf_memcpy_swap.h \
dwarf_memcpy_swap.c \
dwarf_names.c \
dwarf_object_detector.c \
dwarf_object_detector.h \
dwarf_object_read_common.c \
dwarf_object_read_common.h \
dwarf_opaque.h \
dwarf_pe_descr.h \
dwarf_peread.c \
dwarf_peread.h \
dwarf_print_lines.c \
dwarf_query.c \
dwarf_safe_arithmetic.c \
dwarf_safe_arithmetic.h \
dwarf_safe_strcpy.c \
dwarf_safe_strcpy.h \
dwarf_secname_ck.c \
dwarf_secname_ck.h \
dwarf_seekr.c \
dwarf_setup_sections.c \
dwarf_setup_sections.h \
dwarf_ranges.c \
dwarf_rnglists.c \
dwarf_rnglists.h \
dwarf_reading.h \
dwarf_str_offsets.c \
dwarf_str_offsets.h \
dwarf_string.c       \
dwarf_string.h       \
dwarf_stringsection.c \
dwarf_tied.c \
dwarf_tied_decls.h \
dwarf_tsearchhash.c \
dwarf_tsearch.h \
dwarf_universal.h \
dwarf_util.c \
dwarf_util.h \
dwarf_xu_index.c \
dwarf_xu_index.h \
dwarf_local_malloc.c \
dwarf_local_malloc.h \
libdwarf.h \
libdwarf_private.h

libdwarf_la_CPPFLAGS = -DLIBDWARF_BUILD

libdwarf_la_CFLAGS = @ZLIB_CFLAGS@ @ZSTD_CFLAGS@ $(DWARF_CFLAGS_WARN)

libdwarf_la_LIBADD = @DWARF_LIBS@ @ZLIB_LIBS@ @ZSTD_LIBS@

libdwarf_la_LDFLAGS = -fPIC -no-undefined -version-info @version_info@ @release_info@

libdwarfpkgincludedir = $(pkgincludedir)-@VMAJ@
libdwarfpkginclude_DATA = \
$(top_srcdir)/src/lib/libdwarf/dwarf.h  \
$(top_srcdir)/src/lib/libdwarf/libdwarf.h

EXTRA_DIST = \
COPYING \
LGPL.txt \
LIBDWARFCOPYRIGHT \
CHANGES \
ChangeLog \
ChangeLog2006 \
ChangeLog2007 \
ChangeLog2008 \
ChangeLog2009 \
ChangeLog2010 \
ChangeLog2011 \
ChangeLog2012 \
ChangeLog2013 \
ChangeLog2014 \
ChangeLog2015 \
ChangeLog2016 \
ChangeLog2017 \
ChangeLog2018 \
ChangeLog2019 \
ChangeLog2020 \
CODINGSTYLE \
dw-linetableheader.txt \
CMakeLists.txt \
libdwarf.pc.in \
cmake/libdwarfConfig.cmake.in \
meson.build \
NEWS \
README \
CODINGSTYLE
