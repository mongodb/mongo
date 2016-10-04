"""gettext tool
"""


# Copyright (c) 2001 - 2016 The SCons Foundation
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

__revision__ = "src/engine/SCons/Tool/gettext.py rel_2.5.0:3543:937e55cd78f7 2016/04/09 11:29:54 bdbaddog"

#############################################################################
def generate(env,**kw):
  import SCons.Tool
  from SCons.Tool.GettextCommon \
    import  _translate, tool_list
  for t in tool_list(env['PLATFORM'], env):
    env.Tool(t)
  env.AddMethod(_translate, 'Translate')
#############################################################################

#############################################################################
def exists(env):
  from SCons.Tool.GettextCommon \
  import _xgettext_exists, _msginit_exists, \
         _msgmerge_exists, _msgfmt_exists
  try:
    return _xgettext_exists(env) and _msginit_exists(env) \
       and _msgmerge_exists(env) and _msgfmt_exists(env)
  except:
    return False
#############################################################################
