# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""Tool-specific initialization for Docbook.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

import os
import glob
import re

import SCons.Action
import SCons.Builder
import SCons.Defaults
import SCons.Script
import SCons.Tool
import SCons.Util

__debug_tool_location = False
# Get full path to this script
scriptpath = os.path.dirname(os.path.realpath(__file__))

# Local folder for the collection of DocBook XSLs
db_xsl_folder = 'docbook-xsl-1.76.1'

# Do we have lxml?
has_lxml = True
try:
    import lxml
except Exception:
    has_lxml = False

# Set this to True, to prefer xsltproc over lxml
prefer_xsltproc = False

# Regexs for parsing Docbook XML sources of MAN pages
re_manvolnum = re.compile(r"<manvolnum>([^<]*)</manvolnum>")
re_refname = re.compile(r"<refname>([^<]*)</refname>")

#
# lxml etree XSLT global max traversal depth
#

lmxl_xslt_global_max_depth = 3600

if has_lxml and lmxl_xslt_global_max_depth:
    def __lxml_xslt_set_global_max_depth(max_depth) -> None:
        from lxml import etree
        etree.XSLT.set_global_max_depth(max_depth)
    __lxml_xslt_set_global_max_depth(lmxl_xslt_global_max_depth)

#
# Helper functions
#
def __extend_targets_sources(target, source):
    """ Prepare the lists of target and source files. """
    if not SCons.Util.is_List(target):
        target = [target]
    if not source:
        source = target[:]
    elif not SCons.Util.is_List(source):
        source = [source]
    if len(target) < len(source):
        target.extend(source[len(target):])

    return target, source

def __init_xsl_stylesheet(kw, env, user_xsl_var, default_path) -> None:
    if kw.get('DOCBOOK_XSL','') == '':
        xsl_style = kw.get('xsl', env.subst(user_xsl_var))
        if xsl_style == '':
            path_args = [scriptpath, db_xsl_folder] + default_path
            xsl_style = os.path.join(*path_args)
        kw['DOCBOOK_XSL'] =  xsl_style

def __select_builder(lxml_builder, cmdline_builder):
    """ Selects a builder, based on which Python modules are present. """
    if has_lxml and not prefer_xsltproc:
        return lxml_builder

    return cmdline_builder

def __ensure_suffix(t, suffix):
    """ Ensure that the target t has the given suffix. """
    tpath = str(t)
    if not tpath.endswith(suffix):
        return tpath+suffix

    return t

def __ensure_suffix_stem(t, suffix):
    """ Ensure that the target t has the given suffix, and return the file's stem. """
    tpath = str(t)
    if not tpath.endswith(suffix):
        stem = tpath
        tpath += suffix

        return tpath, stem
    else:
        stem, ext = os.path.splitext(tpath)

    return t, stem

def __get_xml_text(root):
    """ Return the text for the given root node (xml.dom.minidom). """
    txt = ""
    for e in root.childNodes:
        if e.nodeType == e.TEXT_NODE:
            txt += e.data
    return txt

def __create_output_dir(base_dir) -> None:
    """ Ensure that the output directory base_dir exists. """
    root, tail = os.path.split(base_dir)
    dir = None
    if tail:
        if base_dir.endswith('/'):
            dir = base_dir
        else:
            dir = root
    else:
        if base_dir.endswith('/'):
            dir = base_dir

    if dir and not os.path.isdir(dir):
        os.makedirs(dir)


#
# Supported command line tools and their call "signature"
#
xsltproc_com_priority = ['xsltproc', 'saxon', 'saxon-xslt', 'xalan']

# TODO: Set minimum version of saxon-xslt to be 8.x (lower than this only supports xslt 1.0.
#       see: https://saxon.sourceforge.net/saxon6.5.5/
#       see: https://saxon.sourceforge.net/
xsltproc_com = {
    'xsltproc': '$DOCBOOK_XSLTPROC $DOCBOOK_XSLTPROCFLAGS -o $TARGET $DOCBOOK_XSL $SOURCE',
    'saxon': '$DOCBOOK_XSLTPROC $DOCBOOK_XSLTPROCFLAGS -o $TARGET $DOCBOOK_XSL $SOURCE $DOCBOOK_XSLTPROCPARAMS',
    # Note if saxon-xslt is version 5.5 the proper arguments are: (swap order of docbook_xsl and source)
    #  'saxon-xslt' : '$DOCBOOK_XSLTPROC $DOCBOOK_XSLTPROCFLAGS -o $TARGET $SOURCE $DOCBOOK_XSL  $DOCBOOK_XSLTPROCPARAMS',
    'saxon-xslt': '$DOCBOOK_XSLTPROC $DOCBOOK_XSLTPROCFLAGS -o $TARGET $DOCBOOK_XSL $SOURCE $DOCBOOK_XSLTPROCPARAMS',
    'xalan': '$DOCBOOK_XSLTPROC $DOCBOOK_XSLTPROCFLAGS -q -out $TARGET -xsl $DOCBOOK_XSL -in $SOURCE',
}
xmllint_com = {
    'xmllint': '$DOCBOOK_XMLLINT $DOCBOOK_XMLLINTFLAGS --xinclude $SOURCE > $TARGET'
}
fop_com = {
    'fop': '$DOCBOOK_FOP $DOCBOOK_FOPFLAGS -fo $SOURCE -pdf $TARGET',
    'xep': '$DOCBOOK_FOP $DOCBOOK_FOPFLAGS -valid -fo $SOURCE -pdf $TARGET',
    'jw': '$DOCBOOK_FOP $DOCBOOK_FOPFLAGS -f docbook -b pdf $SOURCE -o $TARGET',
}

def __detect_cl_tool(env, chainkey, cdict, cpriority=None) -> None:
    """
    Helper function, picks a command line tool from the list
    and initializes its environment variables.
    """
    if env.get(chainkey,'') == '':
        clpath = ''

        if cpriority is None:
            cpriority = cdict.keys()
        for cltool in cpriority:
            if __debug_tool_location:
                print(f"DocBook: Looking for {cltool}")
            clpath = env.WhereIs(cltool)
            if clpath:
                if __debug_tool_location:
                    print(f"DocBook: Found:{cltool}")
                env[chainkey] = clpath
                if not env[chainkey + 'COM']:
                    env[chainkey + 'COM'] = cdict[cltool]
                break

def _detect(env) -> None:
    """
    Detect all the command line tools that we might need for creating
    the requested output formats.
    """
    global prefer_xsltproc

    if env.get('DOCBOOK_PREFER_XSLTPROC',''):
        prefer_xsltproc = True

    if (not has_lxml) or prefer_xsltproc:
        # Try to find the XSLT processors
        __detect_cl_tool(env, 'DOCBOOK_XSLTPROC', xsltproc_com, xsltproc_com_priority)
        __detect_cl_tool(env, 'DOCBOOK_XMLLINT', xmllint_com)

    __detect_cl_tool(env, 'DOCBOOK_FOP', fop_com, ['fop','xep','jw'])

#
# Scanners
#
include_re = re.compile(r'fileref\\s*=\\s*["|\']([^\\n]*)["|\']')
sentity_re = re.compile(r'<!ENTITY\\s+%*\\s*[^\\s]+\\s+SYSTEM\\s+["|\']([^\\n]*)["|\']>')

def __xml_scan(node, env, path, arg):
    """ Simple XML file scanner, detecting local images and XIncludes as implicit dependencies. """
    # Does the node exist yet?
    if not os.path.isfile(str(node)):
        return []

    if env.get('DOCBOOK_SCANENT',''):
        # Use simple pattern matching for system entities..., no support
        # for recursion yet.
        contents = node.get_text_contents()
        return sentity_re.findall(contents)

    xsl_file = os.path.join(scriptpath,'utils','xmldepend.xsl')
    if not has_lxml or prefer_xsltproc:
        # Try to call xsltproc
        xsltproc = env.subst("$DOCBOOK_XSLTPROC")
        if xsltproc and xsltproc.endswith('xsltproc'):
            result = env.backtick(' '.join([xsltproc, xsl_file, str(node)]))
            depfiles = [x.strip() for x in str(result).splitlines() if x.strip() != "" and not x.startswith("<?xml ")]
            return depfiles
        else:
            # Use simple pattern matching, there is currently no support
            # for xi:includes...
            contents = node.get_text_contents()
            return include_re.findall(contents)

    from lxml import etree

    xsl_tree = etree.parse(xsl_file)
    doc = etree.parse(str(node))
    result = doc.xslt(xsl_tree)

    depfiles = [x.strip() for x in str(result).splitlines() if x.strip() != "" and not x.startswith("<?xml ")]
    return depfiles

# Creating the instance of our XML dependency scanner
docbook_xml_scanner = SCons.Script.Scanner(function = __xml_scan,
                                           argument = None)


#
# Action generators
#
def __generate_xsltproc_action(source, target, env, for_signature):
    cmd = env['DOCBOOK_XSLTPROCCOM']
    # Does the environment have a base_dir defined?
    base_dir = env.subst('$base_dir')
    if base_dir:
        # Yes, so replace target path by its filename
        return cmd.replace('$TARGET', os.path.join(base_dir, '${TARGET.file}'))
    return cmd

def __generate_xsltproc_nobase_action(source, target, env, for_signature):
    cmd = env['DOCBOOK_XSLTPROCCOM']
    # Does the environment have a base_dir defined?
    base_dir = env.subst('$base_dir')
    if base_dir:
        # Yes, so replace target path by its filename
        return cmd.replace('$TARGET', '${TARGET.file}')
    return cmd


#
# Emitters
#
def __emit_xsl_basedir(target, source, env):
    # Does the environment have a base_dir defined?
    base_dir = env.subst('$base_dir')
    if base_dir:
        # Yes, so prepend it to each target
        return [os.path.join(base_dir, str(t)) for t in target], source

    # No, so simply pass target and source names through
    return target, source


#
# Builders
#
def __build_lxml(target, source, env):
    """
    General XSLT builder (HTML/FO), using the lxml module.
    """
    from lxml import etree

    xslt_ac = etree.XSLTAccessControl(read_file=True,
                                      write_file=True,
                                      create_dir=True,
                                      read_network=False,
                                      write_network=False)
    xsl_style = env.subst('$DOCBOOK_XSL')
    xsl_tree = etree.parse(xsl_style)
    transform = etree.XSLT(xsl_tree, access_control=xslt_ac)
    doc = etree.parse(str(source[0]))
    # Support for additional parameters
    parampass = {}
    if parampass:
        result = transform(doc, **parampass)
    else:
        result = transform(doc)

    try:
        with open(str(target[0]), "wb") as of:
            of.write(etree.tostring(result, encoding="utf-8", pretty_print=True))
    except Exception as e:
        print(f"ERROR: Failed to write {str(target[0])}")
        print(e)

    return None

def __build_lxml_noresult(target, source, env):
    """
    Specialized XSLT builder for transformations without a direct result where the Docbook
    stylesheet itself creates the target file, using the lxml module.
    """
    from lxml import etree

    xslt_ac = etree.XSLTAccessControl(read_file=True,
                                      write_file=True,
                                      create_dir=True,
                                      read_network=False,
                                      write_network=False)
    xsl_style = env.subst('$DOCBOOK_XSL')
    xsl_tree = etree.parse(xsl_style)
    transform = etree.XSLT(xsl_tree, access_control=xslt_ac)
    doc = etree.parse(str(source[0]))
    # Support for additional parameters
    parampass = {}
    if parampass:
        result = transform(doc, **parampass)
    else:
        result = transform(doc)

    return None


def __xinclude_lxml(target, source, env):
    """
    Resolving XIncludes, using the lxml module.
    """
    from lxml import etree

    doc = etree.parse(str(source[0]))
    doc.xinclude()
    try:
        doc.write(str(target[0]), xml_declaration=True,
                  encoding="UTF-8", pretty_print=True)
    except Exception as e:
        print(f"ERROR: Failed to write {str(target[0])}")
        print(e)

    return None

__lxml_builder = SCons.Builder.Builder(
        action = __build_lxml,
        src_suffix = '.xml',
        source_scanner = docbook_xml_scanner,
        emitter = __emit_xsl_basedir)

__lxml_noresult_builder = SCons.Builder.Builder(
        action = __build_lxml_noresult,
        src_suffix = '.xml',
        source_scanner = docbook_xml_scanner,
        emitter = __emit_xsl_basedir)

__xinclude_lxml_builder = SCons.Builder.Builder(
        action = __xinclude_lxml,
        suffix = '.xml',
        src_suffix = '.xml',
        source_scanner = docbook_xml_scanner)

__xsltproc_builder = SCons.Builder.Builder(
        action = SCons.Action.CommandGeneratorAction(__generate_xsltproc_action,
                                                     {'cmdstr' : '$DOCBOOK_XSLTPROCCOMSTR'}),
        src_suffix = '.xml',
        source_scanner = docbook_xml_scanner,
        emitter = __emit_xsl_basedir)
__xsltproc_nobase_builder = SCons.Builder.Builder(
        action = SCons.Action.CommandGeneratorAction(__generate_xsltproc_nobase_action,
                                                     {'cmdstr' : '$DOCBOOK_XSLTPROCCOMSTR'}),
        src_suffix = '.xml',
        source_scanner = docbook_xml_scanner,
        emitter = __emit_xsl_basedir)
__xmllint_builder = SCons.Builder.Builder(
        action = SCons.Action.Action('$DOCBOOK_XMLLINTCOM','$DOCBOOK_XMLLINTCOMSTR'),
        suffix = '.xml',
        src_suffix = '.xml',
        source_scanner = docbook_xml_scanner)
__fop_builder = SCons.Builder.Builder(
        action = SCons.Action.Action('$DOCBOOK_FOPCOM','$DOCBOOK_FOPCOMSTR'),
        suffix = '.pdf',
        src_suffix = '.fo',
        ensure_suffix=1)

def DocbookEpub(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for ePub output.
    """
    import zipfile
    import shutil

    def build_open_container(target, source, env) -> None:
        """Generate the *.epub file from intermediate outputs

        Constructs the epub file according to the Open Container Format. This
        function could be replaced by a call to the SCons Zip builder if support
        was added for different compression formats for separate source nodes.
        """
        with zipfile.ZipFile(str(target[0]), 'w') as zf:
            with open('mimetype', 'w') as mime_file:
                mime_file.write('application/epub+zip')
            zf.write(mime_file.name, compress_type = zipfile.ZIP_STORED)
            for s in source:
                if os.path.isfile(str(s)):
                    head, tail = os.path.split(str(s))
                    if not head:
                        continue
                    s = head
                for dirpath, dirnames, filenames in os.walk(str(s)):
                    for fname in filenames:
                        path = os.path.join(dirpath, fname)
                        if os.path.isfile(path):
                            zf.write(path, os.path.relpath(path, str(env.get('ZIPROOT', ''))),
                                zipfile.ZIP_DEFLATED)

    def add_resources(target, source, env) -> None:
        """Add missing resources to the OEBPS directory

        Ensure all the resources in the manifest are present in the OEBPS directory.
        """
        hrefs = []
        content_file = os.path.join(source[0].get_abspath(), 'content.opf')
        if not os.path.isfile(content_file):
            return

        hrefs = []
        if has_lxml:
            from lxml import etree

            opf = etree.parse(content_file)
            # All the opf:item elements are resources
            for item in opf.xpath('//opf:item',
                    namespaces= { 'opf': 'http://www.idpf.org/2007/opf' }):
                hrefs.append(item.attrib['href'])

        for href in hrefs:
            # If the resource was not already created by DocBook XSL itself,
            # copy it into the OEBPS folder
            referenced_file = os.path.join(source[0].get_abspath(), href)
            if not os.path.exists(referenced_file):
                shutil.copy(href, os.path.join(source[0].get_abspath(), href))

    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_EPUB', ['epub','docbook.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_noresult_builder, __xsltproc_nobase_builder)

    # Create targets
    result = []
    if not env.GetOption('clean'):
        # Ensure that the folders OEBPS and META-INF exist
        __create_output_dir('OEBPS/')
        __create_output_dir('META-INF/')
    dirs = env.Dir(['OEBPS', 'META-INF'])

    # Set the fixed base_dir
    kw['base_dir'] = 'OEBPS/'
    tocncx = __builder(env, 'toc.ncx', source[0], **kw)
    cxml = env.File('META-INF/container.xml')
    env.SideEffect(cxml, tocncx)

    env.Depends(tocncx, kw['DOCBOOK_XSL'])
    result.extend(tocncx+[cxml])

    container = env.Command(__ensure_suffix(str(target[0]), '.epub'),
        tocncx+[cxml], [add_resources, build_open_container])
    mimetype = env.File('mimetype')
    env.SideEffect(mimetype, container)

    result.extend(container)
    # Add supporting files for cleanup
    env.Clean(tocncx, dirs)

    return result

def DocbookHtml(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for HTML output.
    """
    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_HTML', ['html','docbook.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_builder, __xsltproc_builder)

    # Create targets
    result = []
    for t,s in zip(target,source):
        r = __builder(env, __ensure_suffix(t,'.html'), s, **kw)
        env.Depends(r, kw['DOCBOOK_XSL'])
        result.extend(r)

    return result

def DocbookHtmlChunked(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for chunked HTML output.
    """
    # Init target/source
    if not SCons.Util.is_List(target):
        target = [target]
    if not source:
        source = target
        target = ['index.html']
    elif not SCons.Util.is_List(source):
        source = [source]

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_HTMLCHUNKED', ['html','chunkfast.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_noresult_builder, __xsltproc_nobase_builder)

    # Detect base dir
    base_dir = kw.get('base_dir', '')
    if base_dir:
        __create_output_dir(base_dir)

    # Create targets
    result = []
    r = __builder(env, __ensure_suffix(str(target[0]), '.html'), source[0], **kw)
    env.Depends(r, kw['DOCBOOK_XSL'])
    result.extend(r)
    # Add supporting files for cleanup
    env.Clean(r, glob.glob(os.path.join(base_dir, '*.html')))

    return result


def DocbookHtmlhelp(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for HTMLHELP output.
    """
    # Init target/source
    if not SCons.Util.is_List(target):
        target = [target]
    if not source:
        source = target
        target = ['index.html']
    elif not SCons.Util.is_List(source):
        source = [source]

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_HTMLHELP', ['htmlhelp','htmlhelp.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_noresult_builder, __xsltproc_nobase_builder)

    # Detect base dir
    base_dir = kw.get('base_dir', '')
    if base_dir:
        __create_output_dir(base_dir)

    # Create targets
    result = []
    r = __builder(env, __ensure_suffix(str(target[0]), '.html'), source[0], **kw)
    env.Depends(r, kw['DOCBOOK_XSL'])
    result.extend(r)
    # Add supporting files for cleanup
    env.Clean(r, ['toc.hhc', 'htmlhelp.hhp', 'index.hhk'] +
                 glob.glob(os.path.join(base_dir, '[ar|bk|ch]*.html')))

    return result

def DocbookPdf(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for PDF output.
    """
    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_PDF', ['fo','docbook.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_builder, __xsltproc_builder)

    # Create targets
    result = []
    for t,s in zip(target,source):
        t, stem = __ensure_suffix_stem(t, '.pdf')
        xsl = __builder(env, stem+'.fo', s, **kw)
        result.extend(xsl)
        env.Depends(xsl, kw['DOCBOOK_XSL'])
        result.extend(__fop_builder(env, t, xsl, **kw))

    return result

def DocbookMan(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for Man page output.
    """
    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_MAN', ['manpages','docbook.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_noresult_builder, __xsltproc_builder)

    # Create targets
    result = []
    for t,s in zip(target,source):
        volnum = "1"
        outfiles = []
        srcfile = __ensure_suffix(str(s),'.xml')
        if os.path.isfile(srcfile):
            try:
                import xml.dom.minidom

                dom = xml.dom.minidom.parse(__ensure_suffix(str(s),'.xml'))
                # Extract volume number, default is 1
                for node in dom.getElementsByTagName('refmeta'):
                    for vol in node.getElementsByTagName('manvolnum'):
                        volnum = __get_xml_text(vol)

                # Extract output filenames
                for node in dom.getElementsByTagName('refnamediv'):
                    for ref in node.getElementsByTagName('refname'):
                        outfiles.append(__get_xml_text(ref)+'.'+volnum)

            except Exception:
                # Use simple regex parsing
                with open(__ensure_suffix(str(s), '.xml')) as f:
                    content = f.read()

                for m in re_manvolnum.finditer(content):
                    volnum = m.group(1)

                for m in re_refname.finditer(content):
                    outfiles.append(m.group(1)+'.'+volnum)

            if not outfiles:
                # Use stem of the source file
                spath = str(s)
                if not spath.endswith('.xml'):
                    outfiles.append(spath+'.'+volnum)
                else:
                    stem, ext = os.path.splitext(spath)
                    outfiles.append(stem+'.'+volnum)
        else:
            # We have to completely rely on the given target name
            outfiles.append(t)

        __builder(env, outfiles[0], s, **kw)
        env.Depends(outfiles[0], kw['DOCBOOK_XSL'])
        result.append(outfiles[0])
        if len(outfiles) > 1:
            env.Clean(outfiles[0], outfiles[1:])


    return result

def DocbookSlidesPdf(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for PDF slides output.
    """
    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_SLIDESPDF', ['slides','fo','plain.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_builder, __xsltproc_builder)

    # Create targets
    result = []
    for t,s in zip(target,source):
        t, stem = __ensure_suffix_stem(t, '.pdf')
        xsl = __builder(env, stem+'.fo', s, **kw)
        env.Depends(xsl, kw['DOCBOOK_XSL'])
        result.extend(xsl)
        result.extend(__fop_builder(env, t, xsl, **kw))

    return result

def DocbookSlidesHtml(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, providing a Docbook toolchain for HTML slides output.
    """
    # Init list of targets/sources
    if not SCons.Util.is_List(target):
        target = [target]
    if not source:
        source = target
        target = ['index.html']
    elif not SCons.Util.is_List(source):
        source = [source]

    # Init XSL stylesheet
    __init_xsl_stylesheet(kw, env, '$DOCBOOK_DEFAULT_XSL_SLIDESHTML', ['slides','xhtml','plain.xsl'])

    # Setup builder
    __builder = __select_builder(__lxml_builder, __xsltproc_builder)

    # Detect base dir
    base_dir = kw.get('base_dir', '')
    if base_dir:
        __create_output_dir(base_dir)

    # Create targets
    result = []
    r = __builder(env, __ensure_suffix(str(target[0]), '.html'), source[0], **kw)
    env.Depends(r, kw['DOCBOOK_XSL'])
    result.extend(r)
    # Add supporting files for cleanup
    env.Clean(r, [os.path.join(base_dir, 'toc.html')] +
                 glob.glob(os.path.join(base_dir, 'foil*.html')))

    return result

def DocbookXInclude(env, target, source, *args, **kw):
    """
    A pseudo-Builder, for resolving XIncludes in a separate processing step.
    """
    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Setup builder
    __builder = __select_builder(__xinclude_lxml_builder,__xmllint_builder)

    # Create targets
    result = []
    for t,s in zip(target,source):
        result.extend(__builder(env, t, s, **kw))

    return result

def DocbookXslt(env, target, source=None, *args, **kw):
    """
    A pseudo-Builder, applying a simple XSL transformation to the input file.
    """
    # Init list of targets/sources
    target, source = __extend_targets_sources(target, source)

    # Init XSL stylesheet
    kw['DOCBOOK_XSL'] = env.File(kw.get('xsl', 'transform.xsl'))

    # Setup builder
    __builder = __select_builder(__lxml_builder, __xsltproc_builder)

    # Create targets
    result = []
    for t,s in zip(target,source):
        r = __builder(env, t, s, **kw)
        env.Depends(r, kw['DOCBOOK_XSL'])
        result.extend(r)

    return result


def generate(env) -> None:
    """Add Builders and construction variables for docbook to an Environment."""

    env.SetDefault(
        # Default names for customized XSL stylesheets
        DOCBOOK_DEFAULT_XSL_EPUB = '',
        DOCBOOK_DEFAULT_XSL_HTML = '',
        DOCBOOK_DEFAULT_XSL_HTMLCHUNKED = '',
        DOCBOOK_DEFAULT_XSL_HTMLHELP = '',
        DOCBOOK_DEFAULT_XSL_PDF = '',
        DOCBOOK_DEFAULT_XSL_MAN = '',
        DOCBOOK_DEFAULT_XSL_SLIDESPDF = '',
        DOCBOOK_DEFAULT_XSL_SLIDESHTML = '',

        # Paths to the detected executables
        DOCBOOK_XSLTPROC = '',
        DOCBOOK_XMLLINT = '',
        DOCBOOK_FOP = '',

        # Additional flags for the text processors
        DOCBOOK_XSLTPROCFLAGS = SCons.Util.CLVar(''),
        DOCBOOK_XMLLINTFLAGS = SCons.Util.CLVar(''),
        DOCBOOK_FOPFLAGS = SCons.Util.CLVar(''),
        DOCBOOK_XSLTPROCPARAMS = SCons.Util.CLVar(''),

        # Default command lines for the detected executables
        DOCBOOK_XSLTPROCCOM = xsltproc_com['xsltproc'],
        DOCBOOK_XMLLINTCOM = xmllint_com['xmllint'],
        DOCBOOK_FOPCOM = fop_com['fop'],

        # Screen output for the text processors
        DOCBOOK_XSLTPROCCOMSTR = None,
        DOCBOOK_XMLLINTCOMSTR = None,
        DOCBOOK_FOPCOMSTR = None,

        )
    _detect(env)

    env.AddMethod(DocbookEpub, "DocbookEpub")
    env.AddMethod(DocbookHtml, "DocbookHtml")
    env.AddMethod(DocbookHtmlChunked, "DocbookHtmlChunked")
    env.AddMethod(DocbookHtmlhelp, "DocbookHtmlhelp")
    env.AddMethod(DocbookPdf, "DocbookPdf")
    env.AddMethod(DocbookMan, "DocbookMan")
    env.AddMethod(DocbookSlidesPdf, "DocbookSlidesPdf")
    env.AddMethod(DocbookSlidesHtml, "DocbookSlidesHtml")
    env.AddMethod(DocbookXInclude, "DocbookXInclude")
    env.AddMethod(DocbookXslt, "DocbookXslt")


def exists(env) -> bool:
    return True
