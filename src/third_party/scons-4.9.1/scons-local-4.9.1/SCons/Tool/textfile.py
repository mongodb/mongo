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

"""Textfile/Substfile builder for SCons.

Create file 'target' which typically is a textfile.  The 'source'
may be any combination of strings, Nodes, or lists of same.  A
'linesep' will be put between any part written and defaults to
os.linesep.

The only difference between the Textfile builder and the Substfile
builder is that strings are converted to Value() nodes for the
former and File() nodes for the latter.  To insert files in the
former or strings in the latter, wrap them in a File() or Value(),
respectively.

The values of SUBST_DICT first have any construction variables
expanded (its keys are not expanded).  If a value of SUBST_DICT is
a python callable function, it is called and the result is expanded
as the value.  Values are substituted in a "random" order; if any
substitution could be further expanded by another substitution, it
is unpredictable whether the expansion will occur.
"""

import SCons
from SCons.Node import Node
from SCons.Node.Python import Value
from SCons.Util import is_String, is_Sequence, is_Dict, to_bytes


TEXTFILE_FILE_WRITE_MODE = 'w'

LINESEP = '\n'


def _do_subst(node, subs):
    """
    Fetch the node contents and replace all instances of the keys with
    their values.  For example, if subs is
        {'%VERSION%': '1.2345', '%BASE%': 'MyProg', '%prefix%': '/bin'},
    then all instances of %VERSION% in the file will be replaced with
    1.2345 and so forth.
    """
    contents = node.get_text_contents()
    if subs:
        for (k, val) in subs:
            contents = contents.replace(k, val)

    if 'b' in TEXTFILE_FILE_WRITE_MODE:
        try:
            contents = bytearray(contents, 'utf-8')
        except TypeError:
            # TODO: this should not happen, get_text_contents returns text
            contents = bytearray(contents)

    return contents


def _action(target, source, env):

    # prepare the line separator
    linesep = env['LINESEPARATOR']
    if linesep is None:
        linesep = LINESEP  # os.linesep
    elif is_String(linesep):
        pass
    elif isinstance(linesep, Value):
        linesep = linesep.get_text_contents()
    else:
        raise SCons.Errors.UserError('unexpected type/class for LINESEPARATOR: %s'
                                     % repr(linesep), None)

    if 'b' in TEXTFILE_FILE_WRITE_MODE:
        linesep = to_bytes(linesep)

    # create a dictionary to use for the substitutions
    if 'SUBST_DICT' not in env:
        subs = None    # no substitutions
    else:
        subst_dict = env['SUBST_DICT']
        if is_Dict(subst_dict):
            subst_dict = list(subst_dict.items())
        elif is_Sequence(subst_dict):
            pass
        else:
            raise SCons.Errors.UserError('SUBST_DICT must be dict or sequence')
        subs = []
        for (k, value) in subst_dict:
            if callable(value):
                value = value()
            if is_String(value):
                value = env.subst(value, raw=1)
            else:
                value = str(value)
            subs.append((k, value))

    # Pull file encoding from the environment or default to UTF-8
    file_encoding = env.get('FILE_ENCODING', 'utf-8')

    # write the file
    try:
        target_file = open(target[0].get_path(), TEXTFILE_FILE_WRITE_MODE, newline='', encoding=file_encoding)
    except OSError as e:
        raise SCons.Errors.UserError("Can't write target file %s [%s]" % (target[0],e))

    # separate lines by 'linesep' only if linesep is not empty
    lsep = None
    for line in source:
        if lsep:
            target_file.write(lsep)

        target_file.write(_do_subst(line, subs))
        lsep = linesep
    target_file.close()


def _strfunc(target, source, env) -> str:
    return "Creating '%s'" % target[0]


def _convert_list_R(newlist, sources) -> None:
    for elem in sources:
        if is_Sequence(elem):
            _convert_list_R(newlist, elem)
        elif isinstance(elem, Node):
            newlist.append(elem)
        else:
            newlist.append(Value(elem))


def _convert_list(target, source, env):
    if len(target) != 1:
        raise SCons.Errors.UserError("Only one target file allowed")
    newlist = []
    _convert_list_R(newlist, source)
    return target, newlist


_common_varlist = ['SUBST_DICT', 'LINESEPARATOR']

_text_varlist = _common_varlist + ['TEXTFILEPREFIX', 'TEXTFILESUFFIX']
_text_builder = SCons.Builder.Builder(
    action=SCons.Action.Action(_action, _strfunc, varlist=_text_varlist),
    source_factory=Value,
    emitter=_convert_list,
    prefix='$TEXTFILEPREFIX',
    suffix='$TEXTFILESUFFIX',
)

_subst_varlist = _common_varlist + ['SUBSTFILEPREFIX', 'SUBSTFILESUFFIX']
_subst_builder = SCons.Builder.Builder(
    action=SCons.Action.Action(_action, _strfunc, varlist=_subst_varlist),
    source_factory=SCons.Node.FS.File,
    emitter=_convert_list,
    prefix='$SUBSTFILEPREFIX',
    suffix='$SUBSTFILESUFFIX',
    src_suffix=['.in'],
)


def generate(env) -> None:
    env['LINESEPARATOR'] = LINESEP # os.linesep
    env['BUILDERS']['Textfile'] = _text_builder
    env['TEXTFILEPREFIX'] = ''
    env['TEXTFILESUFFIX'] = '.txt'
    env['BUILDERS']['Substfile'] = _subst_builder
    env['SUBSTFILEPREFIX'] = ''
    env['SUBSTFILESUFFIX'] = ''
    env['FILE_ENCODING'] = env.get('FILE_ENCODING', 'utf-8')


def exists(env) -> bool:
    return True

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
