""" msgmerget tool 

Tool specific initialization for `msgmerge` tool.
"""

# Copyright (c) 2001 - 2019 The SCons Foundation
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

__revision__ = "src/engine/SCons/Tool/msgmerge.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

#############################################################################
def _update_or_init_po_files(target, source, env):
  """ Action function for `POUpdate` builder """
  import SCons.Action
  from SCons.Tool.GettextCommon import _init_po_files
  for tgt in target:
    if tgt.rexists():
      action = SCons.Action.Action('$MSGMERGECOM', '$MSGMERGECOMSTR')
    else:
      action = _init_po_files
    status = action([tgt], source, env)
    if status : return status
  return 0
#############################################################################

#############################################################################
def _POUpdateBuilder(env, **kw):
  """ Create an object of `POUpdate` builder """
  import SCons.Action
  from SCons.Tool.GettextCommon import _POFileBuilder
  action = SCons.Action.Action(_update_or_init_po_files, None)
  return _POFileBuilder(env, action=action, target_alias='$POUPDATE_ALIAS')
#############################################################################

#############################################################################
from SCons.Environment import _null
#############################################################################
def _POUpdateBuilderWrapper(env, target=None, source=_null, **kw):
  """ Wrapper for `POUpdate` builder - make user's life easier """
  if source is _null:
    if 'POTDOMAIN' in kw:
      domain = kw['POTDOMAIN']
    elif 'POTDOMAIN' in env and env['POTDOMAIN']:
      domain = env['POTDOMAIN']
    else:
      domain = 'messages'
    source = [ domain ] # NOTE: Suffix shall be appended automatically
  return env._POUpdateBuilder(target, source, **kw)
#############################################################################

#############################################################################
def generate(env,**kw):
  """ Generate the `msgmerge` tool """
  import sys
  import os
  import SCons.Tool
  from SCons.Tool.GettextCommon import _detect_msgmerge
  from SCons.Platform.mingw import MINGW_DEFAULT_PATHS
  from SCons.Platform.cygwin import CYGWIN_DEFAULT_PATHS

  if sys.platform == 'win32':
      msgmerge = SCons.Tool.find_program_path(env, 'msgmerge', default_paths=MINGW_DEFAULT_PATHS + CYGWIN_DEFAULT_PATHS )
      if msgmerge:
          msgmerge_bin_dir = os.path.dirname(msgmerge)
          env.AppendENVPath('PATH', msgmerge_bin_dir)
      else:
          SCons.Warnings.Warning('msgmerge tool requested, but binary not found in ENV PATH')
  try:
    env['MSGMERGE'] = _detect_msgmerge(env)
  except:
    env['MSGMERGE'] = 'msgmerge'
  env.SetDefault(
    POTSUFFIX = ['.pot'],
    POSUFFIX = ['.po'],
    MSGMERGECOM = '$MSGMERGE  $MSGMERGEFLAGS --update $TARGET $SOURCE',
    MSGMERGECOMSTR = '',
    MSGMERGEFLAGS = [ ],
    POUPDATE_ALIAS = 'po-update'
  )
  env.Append(BUILDERS = { '_POUpdateBuilder':_POUpdateBuilder(env) })
  env.AddMethod(_POUpdateBuilderWrapper, 'POUpdate')
  env.AlwaysBuild(env.Alias('$POUPDATE_ALIAS'))
#############################################################################

#############################################################################
def exists(env):
  """ Check if the tool exists """
  from SCons.Tool.GettextCommon import _msgmerge_exists
  try:
    return  _msgmerge_exists(env)
  except:
    return False
#############################################################################

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
