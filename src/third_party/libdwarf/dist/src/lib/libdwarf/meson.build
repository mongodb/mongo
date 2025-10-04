
libdwarf_header_src = [ 'libdwarf.h', 'dwarf.h' ]

libdwarf_src = [
  'dwarf_abbrev.c',
  'dwarf_alloc.c',
  'dwarf_arange.c',
  'dwarf_crc.c',
  'dwarf_crc32.c',
  'dwarf_debugaddr.c',
  'dwarf_debuglink.c',
  'dwarf_die_deliv.c',
  'dwarf_debugnames.c',
  'dwarf_debug_sup.c',
  'dwarf_dsc.c',
  'dwarf_elf_load_headers.c',
  'dwarf_elfread.c',
  'dwarf_elf_rel_detector.c',
  'dwarf_error.c',
  'dwarf_fill_in_attr_form.c',
  'dwarf_find_sigref.c',
  'dwarf_fission_to_cu.c',
  'dwarf_form.c',
  'dwarf_form_class_names.c',
  'dwarf_frame.c',
  'dwarf_frame2.c',
  'dwarf_gdbindex.c',
  'dwarf_generic_init.c',
  'dwarf_global.c',
  'dwarf_gnu_index.c',
  'dwarf_groups.c',
  'dwarf_harmless.c',
  'dwarf_init_finish.c',
  'dwarf_leb.c',
  'dwarf_line.c',
  'dwarf_loc.c',
  'dwarf_locationop_read.c',
  'dwarf_local_malloc.c',
  'dwarf_loclists.c',
  'dwarf_machoread.c',
  'dwarf_macro.c',
  'dwarf_macro5.c',
  'dwarf_memcpy_swap.c',
  'dwarf_names.c',
  'dwarf_object_detector.c',
  'dwarf_object_read_common.c',
  'dwarf_peread.c',
  'dwarf_print_lines.c',
  'dwarf_query.c',
  'dwarf_safe_arithmetic.c',
  'dwarf_safe_strcpy.c',
  'dwarf_secname_ck.c',
  'dwarf_setup_sections.c',
  'dwarf_ranges.c',
  'dwarf_rnglists.c',
  'dwarf_seekr.c',
  'dwarf_str_offsets.c',
  'dwarf_string.c',
  'dwarf_stringsection.c',
  'dwarf_tied.c',
  'dwarf_tsearchhash.c',
  'dwarf_util.c',
  'dwarf_xu_index.c',
]

if dev_decompression
    zlib_deps = dependency('zlib', method: 'pkg-config', required: false)
    libzstd_deps = dependency('libzstd', method: 'pkg-config', required: false)

    if not zlib_deps.found()
        if fs.is_dir('/opt/local/lib') and fs.is_file('/opt/local/include/zlib.h')
          zlib_deps = declare_dependency(link_args: [ '-L/opt/local/lib','-lz' ],
            include_directories: ['/opt/local/include'])
        endif
    endif

    if not libzstd_deps.found()
        if fs.is_dir('/opt/local/lib') and fs.is_file('/opt/local/include/zstd.h')
            libzstd_deps = declare_dependency(link_args: ['-L/opt/local/lib','-lzstd' ],
             include_directories: ['/opt/local/include'])
        endif
    endif

    if zlib_deps.found()
          message(['Have zlib'])
    else
        if fs.is_dir('/usr/local/lib') and fs.is_file('/usr/local/include/zlib.h')
          zlib_deps = declare_dependency(link_args: [ '-L/usr/local/lib','-lz' ],
              include_directories: ['/usr/local/include'])
        endif
    endif

    if libzstd_deps.found()
        message(['Have libzstd'])
    else
        if fs.is_dir('/usr/local/lib') and fs.is_file('/usr/local/include/zstd.h')
            libzstd_deps = declare_dependency(link_args: [ '-L/usr/local/lib',
              '-lzstd' ],
              include_directories: ['/usr/local/include'])
      endif
    endif

    # Using set10 as false has the wrong effect, does not
    # match what compilers expect from #ifdef in C.
    if zlib_deps.found()
        if libzstd_deps.found()
            config_h.set10('HAVE_ZSTD_H',true)
            config_h.set10('HAVE_ZSTD',true)
            config_h.set10('HAVE_ZLIB_H',true)
            config_h.set10('HAVE_ZLIB',true)
        else
            zlib_deps = dependency('',required: false)
        endif
    else
        if libzstd_deps.found()
            libszstd_deps = dependency('',required: false)
        endif
    endif
else
    zlib_deps = dependency('',required: false)
    libzstd_deps = dependency('',required: false)
endif

if (lib_type == 'shared')
  compiler_flags = ['-DLIBDWARF_BUILD']
  compiler_flags_public = []
else
  compiler_flags = ['-DLIBDWARF_STATIC']
  compiler_flags_public = ['-DLIBDWARF_STATIC']
endif

libdwarf_lib = library('dwarf', libdwarf_src,
  c_args : [ dev_cflags,dev_cppflags_onlylibdwarf, libdwarf_args, compiler_flags ],
  dependencies : [ zlib_deps, libzstd_deps ],
  gnu_symbol_visibility: 'hidden',
  include_directories : config_dir,
  install : true,
  pic : true,
  version : meson.project_version()
)

libdwarf = declare_dependency(
  include_directories : [ include_directories('.')],
  link_with : libdwarf_lib,
  compile_args : compiler_flags_public,
  dependencies : [zlib_deps, libzstd_deps]
)

meson.override_dependency('libdwarf', libdwarf)

install_headers(libdwarf_header_src,
  install_dir : dir_pkginclude + '-' + v_maj
)
