"""SCons.Platform.virtualenv

Support for virtualenv.
"""

#
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
#

__revision__ = "src/engine/SCons/Platform/virtualenv.py bee7caf9defd6e108fc2998a2520ddb36a967691 2019-12-17 02:07:09 bdeegan"

import os
import sys
import SCons.Util


virtualenv_enabled_by_default = False


def _enable_virtualenv_default():
    return SCons.Util.get_os_env_bool('SCONS_ENABLE_VIRTUALENV', virtualenv_enabled_by_default)


def _ignore_virtualenv_default():
    return SCons.Util.get_os_env_bool('SCONS_IGNORE_VIRTUALENV', False)


enable_virtualenv = _enable_virtualenv_default()
ignore_virtualenv = _ignore_virtualenv_default()
virtualenv_variables = ['VIRTUAL_ENV', 'PIPENV_ACTIVE']


def _running_in_virtualenv():
    """Returns True, if scons is executed within a virtualenv"""
    # see https://stackoverflow.com/a/42580137
    return (hasattr(sys, 'real_prefix') or
            (hasattr(sys, 'base_prefix') and sys.base_prefix != sys.prefix))


def _is_path_in(path, base):
    """Returns true, if **path** is located under the **base** directory."""
    if not path or not base: # empty path may happen, base too
        return False
    rp = os.path.relpath(path, base)
    return ((not rp.startswith(os.path.pardir)) and (not rp == os.path.curdir))


def _inject_venv_variables(env):
    if 'ENV' not in env:
        env['ENV'] = {}
    ENV = env['ENV']
    for name in virtualenv_variables:
        try:
            ENV[name] = os.environ[name]
        except KeyError:
            pass

def _inject_venv_path(env, path_list=None):
    """Modify environment such that SCons will take into account its virtualenv
    when running external tools."""
    if path_list is None:
        path_list = os.getenv('PATH')
    env.PrependENVPath('PATH', select_paths_in_venv(path_list))


def select_paths_in_venv(path_list):
    """Returns a list of paths from **path_list** which are under virtualenv's
    home directory."""
    if SCons.Util.is_String(path_list):
        path_list = path_list.split(os.path.pathsep)
    # Find in path_list the paths under the virtualenv's home
    return [path for path in path_list if IsInVirtualenv(path)]


def ImportVirtualenv(env):
    """Copies virtualenv-related environment variables from OS environment
    to ``env['ENV']`` and prepends virtualenv's PATH to ``env['ENV']['PATH']``.
    """
    _inject_venv_variables(env)
    _inject_venv_path(env)


def Virtualenv():
    """Returns path to the virtualenv home if scons is executing within a
    virtualenv or None, if not."""
    if _running_in_virtualenv():
        return sys.prefix
    return None


def IsInVirtualenv(path):
    """Returns True, if **path** is under virtualenv's home directory. If not,
    or if we don't use virtualenv, returns False."""
    return _is_path_in(path, Virtualenv())


# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
