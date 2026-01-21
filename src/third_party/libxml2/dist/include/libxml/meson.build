
## xmlversion.h
xmlversion_h = configuration_data()
xmlversion_h.set('VERSION', meson.project_version())
xmlversion_h.set('LIBXML_VERSION_NUMBER', v_nbr.to_string())
xmlversion_h.set('LIBXML_VERSION_EXTRA', v_extra)
xmlversion_h.set10('WITH_C14N', want_c14n)
xmlversion_h.set10('WITH_CATALOG', want_catalog)
xmlversion_h.set10('WITH_DEBUG', want_debug)
xmlversion_h.set10('WITH_HTML', want_html)
xmlversion_h.set10('WITH_HTTP', want_http)
xmlversion_h.set10('WITH_ICONV', want_iconv)
xmlversion_h.set10('WITH_ICU', want_icu)
xmlversion_h.set10('WITH_ISO8859X', want_iso8859x)
xmlversion_h.set10('WITH_LEGACY', want_legacy)
xmlversion_h.set10('WITH_MODULES', want_modules)
xmlversion_h.set('MODULE_EXTENSION', module_extension)
xmlversion_h.set10('WITH_OUTPUT', want_output)
xmlversion_h.set10('WITH_PATTERN', want_pattern)
xmlversion_h.set10('WITH_PUSH', want_push)
xmlversion_h.set10('WITH_READER', want_reader)
xmlversion_h.set10('WITH_REGEXPS', want_regexps)
xmlversion_h.set10('WITH_RELAXNG', want_relaxng)
xmlversion_h.set10('WITH_SAX1', want_sax1)
xmlversion_h.set10('WITH_SCHEMAS', want_schemas)
xmlversion_h.set10('WITH_SCHEMATRON', want_schematron)
xmlversion_h.set10('WITH_THREADS', want_threads)
xmlversion_h.set10('WITH_THREAD_ALLOC', want_thread_alloc)
xmlversion_h.set10('WITH_VALID', want_valid)
xmlversion_h.set10('WITH_WRITER', want_writer)
xmlversion_h.set10('WITH_XINCLUDE', want_xinclude)
xmlversion_h.set10('WITH_XPATH', want_xpath)
xmlversion_h.set10('WITH_XPTR', want_xptr)
xmlversion_h.set10('WITH_ZLIB', want_zlib)

configure_file(
    input: 'xmlversion.h.in',
    output: 'xmlversion.h',
    configuration: xmlversion_h,
    install_dir: dir_pkginclude + '/libxml',
)

#vcs_tag(
#  command : [ 'git', 'describe', '2>/dev/null' ],
#  input : 'xmlversion.h.in',
#  output : 'xmlversion.h',
#  replace_string : 'LIBXML_VERSION_EXTRA'
#)

libxml_headers = files(
    'HTMLparser.h',
    'HTMLtree.h',
    'SAX.h',
    'SAX2.h',
    'c14n.h',
    'catalog.h',
    'chvalid.h',
    'debugXML.h',
    'dict.h',
    'encoding.h',
    'entities.h',
    'globals.h',
    'hash.h',
    'list.h',
    'nanoftp.h',
    'nanohttp.h',
    'parser.h',
    'parserInternals.h',
    'pattern.h',
    'relaxng.h',
    'schemasInternals.h',
    'schematron.h',
    'threads.h',
    'tree.h',
    'uri.h',
    'valid.h',
    'xinclude.h',
    'xlink.h',
    'xmlIO.h',
    'xmlautomata.h',
    'xmlerror.h',
    'xmlexports.h',
    'xmlmemory.h',
    'xmlmodule.h',
    'xmlreader.h',
    'xmlregexp.h',
    'xmlsave.h',
    'xmlschemas.h',
    'xmlschemastypes.h',
    'xmlstring.h',
    'xmlunicode.h',
    'xmlwriter.h',
    'xpath.h',
    'xpathInternals.h',
    'xpointer.h',
)

install_headers(libxml_headers, install_dir: dir_pkginclude / 'libxml')
