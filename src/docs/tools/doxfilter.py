#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.


# This Python script is run as part of generating the documentation for the
# WiredTiger reference manual.  It changes comments to Javadoc style
# (i.e., from "/*!" to "/**"), because the latter are configured to not
# search for brief descriptions at the beginning of pages.
# It also processes any page marked with @m_page specially to create
# multiple per-language versions of the page.

import re, sys

progname = 'doxfilter.py'
linenum = 0
filename = '<unknown>'

def err(arg):
    sys.stderr.write(filename + ':' + str(linenum) + ': ERROR: ' + arg + '\n')
    sys.exit(1)

def java_post_substitutions(source):
    result = source
    for datatype in [['WT_CONNECTION', 'Connection'],
             ['WT_CURSOR', 'Cursor'],
             ['WT_SESSION', 'Session'],
             ['WT_ASYNC_OPTYPE', 'AsyncOpType'],
             ['WT_ASYNC_OP', 'AsyncOp']]:
        fromdt = datatype[0]
        todt = datatype[1]

        # e.g. replace("WT_CONNECTION::", "Connection.").
        #      replace("WT_CONNECTION", "Connection")
        #      replace("::WT_CONNECTION", "Connection")
        # etc.
        result = result.replace(fromdt + '::', todt + '.')
        result = re.sub(r':*' + fromdt, todt, result)

        # We fix back any 'ref' entries, since we don't have
        # many java versions of these refered-to pages.
        #
        # Ideally, we'd have a way ('@m_ref'?) to indicate which
        # ones could refer to a Java version.
        result = result.replace('ref ' + todt + '.', 'ref ' + fromdt + '::')
    result = result.replace('::wiredtiger_open', '\c wiredtiger.open')
    return result

def process_lang(lang, lines):
    result = ''
    lang_ext = '.' + lang
    class_suffix = None
    if lang == 'c':
        lang_suffix = ""
        lang_desc=""
    elif lang == 'java':
        lang_suffix = "_lang_java"
        lang_desc=" in Java"
    else:
        err('@m_page contains illegal lang: ' + lang)
    condstack = [True]
    linenum = 0
    mif_pat = re.compile(r'^\s*@m_if{([^,}]*)}')
    melse_pat = re.compile(r'^\s*@m_else\s*$')
    mendif_pat = re.compile(r'^\s*@m_endif\s*$')
    mpage_pat = re.compile(r'@m_page{{([^}]*)},([^,}]*),([^}]*)}')
    mpage_rep = r'@page \2' + lang_suffix + r' \3 ' + lang_desc
    ref_pat = re.compile(r'@ref\s+(\w*)')
    ref_rep = r'@ref \1' + lang_suffix
    snip_pat = re.compile(r'@snippet ex_([^.]*)[.]c\s+(.*)')
    snip_rep = r'@snippet ex_\1' + lang_ext + r' \2'
    section_pat = re.compile(r'(^@\w*section)\s+(\w*)')
    section_rep = r'\1 \2' + lang_suffix
    subpage_pat = re.compile(r'@subpage\s+(\w*)')
    subpage_rep = r'@subpage \1' + lang_suffix
    exref_pat = re.compile(r'@ex_ref{ex_([^.]*)[.]c}')
    if lang == 'c':
        exref_rep = r'@ex_ref{ex_\1' + lang_ext + '}'
    else:
        # Though we have java examples, we don't have references
        # to them working yet, so strip the @ex_ref.
        exref_rep = r'ex_\1' + lang_ext

    # Any remaining @m_foo{...} aliases are
    # diverted to @c_foo{...} or @java_foo{...}
    mgeneric_pat = re.compile(r'@m_([^ }]*)')
    mgeneric_rep = r'@' + lang + r'_\1'
    for line in lines:
        linenum += 1
        if lang != 'c':
            line = re.sub(exref_pat, exref_rep, line)
            line = re.sub(ref_pat, ref_rep, line)
            line = re.sub(section_pat, section_rep, line)
            line = re.sub(snip_pat, snip_rep, line)
        line = re.sub(mpage_pat, mpage_rep, line)
        line = re.sub(subpage_pat, subpage_rep, line)
        if '@m_if' in line:
            m = re.search(mif_pat, line)
            if not m:
                err('@m_if incorrect syntax')
            iflang = m.groups()[0]
            if iflang != 'java' and iflang != 'c':
                err('@m_if unknown language')
            condstack.append(iflang == lang)
        elif '@m_else' in line:
            if not re.search(melse_pat, line):
                err('@m_else has extraneous stuff')
            if len(condstack) <= 1:
                err('@m_else missing if')
            condstack[-1] = not condstack[-1]
        elif '@m_endif' in line:
            if not re.search(mendif_pat, line):
                err('@m_endif has extraneous stuff')
            if len(condstack) <= 1:
                err('@m_endif missing if')
            condstack.pop()
        else:
            if condstack[-1]:
                # Do generic @m_... macros last
                line = re.sub(mgeneric_pat,
                          mgeneric_rep, line)
                result += line + '\n'
    if lang == 'java':
        result = java_post_substitutions(result)
    if len(condstack) != 1:
        err('non matching @m_if/@m_endif')
    return result

# Collect all lines between @m_page and the comment end to
# be processed multiple times, once for each language.
def process_multilang(source):
    result = ''
    in_mpage = False
    mpage_content = []
    mpage_pat = re.compile(r'@m_page{{([^}]*)},([^,}]*),([^}]*)}')
    mpage_end_pat = re.compile(r'\*/')
    for line in source.split('\n'):
        m = re.search(mpage_pat, line)
        if line.count('@m_page') > 0 and not m:
            err('@m_page incorrect syntax')
        if m:
            if in_mpage:
                err('multiple @m_page without end of comment')
            else:
                in_mpage = True
                langs = m.groups()[0].split(',')
        if in_mpage:
            mpage_content.append(line)
        else:
            result += line + '\n'
        if re.search(mpage_end_pat, line):
            if in_mpage:
                in_mpage = False
                for lang in langs:
                    result += process_lang(lang, mpage_content)
                mpage_content = []
    return result

def process(source):
    source = source.replace(r'/*!', r'/**')
    if '@m_' in source:
        source = process_multilang(source)
    return source

if __name__ == '__main__':
    for f in sys.argv[1:]:
        filename = f
        with open(f, 'r') as infile:
            sys.stdout.write(process(infile.read()))
        sys.exit(0)
