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

"""
Determine if and/or when an error/warning should be issued when there
are no versions of msvc installed.  If there is at least one version of
msvc installed, these routines do (almost) nothing.

Notes:
    * When msvc is the default compiler because there are no compilers
      installed, a build may fail due to the cl.exe command not being
      recognized.  Currently, there is no easy way to detect during
      msvc initialization if the default environment will be used later
      to build a program and/or library. There is no error/warning
      as there are legitimate SCons uses that do not require a c compiler.
    * An error is indicated by returning a non-empty tool list from the
      function register_iserror.
"""

import re

from .. common import (
    debug,
)

from . import Dispatcher
Dispatcher.register_modulename(__name__)


class _Data:

    separator = r';'

    need_init = True

    @classmethod
    def reset(cls) -> None:
        debug('msvc default:init')
        cls.n_setup = 0                 # number of calls to msvc_setup_env_once
        cls.default_ismsvc = False      # is msvc the default compiler
        cls.default_tools_re_list = []  # list of default tools regular expressions
        cls.msvc_tools_init = set()     # tools registered via msvc_exists
        cls.msvc_tools = None           # tools registered via msvc_setup_env_once
        cls.msvc_installed = False      # is msvc installed (vcs_installed > 0)
        cls.msvc_nodefault = False      # is there a default version of msvc
        cls.need_init = True            # reset initialization indicator

def _initialize(env, msvc_exists_func) -> None:
    if _Data.need_init:
        _Data.reset()
        _Data.need_init = False
        _Data.msvc_installed = msvc_exists_func(env)
        debug('msvc default:msvc_installed=%s', _Data.msvc_installed)

def register_tool(env, tool, msvc_exists_func):
    if _Data.need_init:
        _initialize(env, msvc_exists_func)
    if _Data.msvc_installed:
        return None
    if not tool:
        return None
    if _Data.n_setup == 0:
        if tool not in _Data.msvc_tools_init:
            _Data.msvc_tools_init.add(tool)
            debug('msvc default:tool=%s, msvc_tools_init=%s', tool, _Data.msvc_tools_init)
        return None
    if tool not in _Data.msvc_tools:
        _Data.msvc_tools.add(tool)
        debug('msvc default:tool=%s, msvc_tools=%s', tool, _Data.msvc_tools)

def register_setup(env, msvc_exists_func) -> None:
    if _Data.need_init:
        _initialize(env, msvc_exists_func)
    _Data.n_setup += 1
    if not _Data.msvc_installed:
        _Data.msvc_tools = set(_Data.msvc_tools_init)
        if _Data.n_setup == 1:
            tool_list = env.get('TOOLS', None)
            if tool_list and tool_list[0] == 'default':
                if len(tool_list) > 1 and tool_list[1] in _Data.msvc_tools:
                    # msvc tools are the default compiler
                    _Data.default_ismsvc = True
        _Data.msvc_nodefault = False
        debug(
            'msvc default:n_setup=%d, msvc_installed=%s, default_ismsvc=%s',
            _Data.n_setup, _Data.msvc_installed, _Data.default_ismsvc
        )

def set_nodefault() -> None:
    # default msvc version, msvc not installed
    _Data.msvc_nodefault = True
    debug('msvc default:msvc_nodefault=%s', _Data.msvc_nodefault)

def register_iserror(env, tool, msvc_exists_func):

    register_tool(env, tool, msvc_exists_func)

    if _Data.msvc_installed:
        # msvc installed
        return None

    if not _Data.msvc_nodefault:
        # msvc version specified
        return None

    tool_list = env.get('TOOLS', None)
    if not tool_list:
        # tool list is empty
        return None

    debug(
        'msvc default:n_setup=%s, default_ismsvc=%s, msvc_tools=%s, tool_list=%s',
        _Data.n_setup, _Data.default_ismsvc, _Data.msvc_tools, tool_list
    )

    if not _Data.default_ismsvc:

        # Summary:
        #    * msvc is not installed
        #    * msvc version not specified (default)
        #    * msvc is not the default compiler

        # construct tools set
        tools_set = set(tool_list)

    else:

        if _Data.n_setup == 1:
            # first setup and msvc is default compiler:
            #     build default tools regex for current tool state
            tools = _Data.separator.join(tool_list)
            tools_nchar = len(tools)
            debug('msvc default:add regex:nchar=%d, tools=%s', tools_nchar, tools)
            re_default_tools = re.compile(re.escape(tools))
            _Data.default_tools_re_list.insert(0, (tools_nchar, re_default_tools))
            # early exit: no error for default environment when msvc is not installed
            return None

        # Summary:
        #    * msvc is not installed
        #    * msvc version not specified (default)
        #    * environment tools list is not empty
        #    * default tools regex list constructed
        #    * msvc tools set constructed
        #
        # Algorithm using tools string and sets:
        #    * convert environment tools list to a string
        #    * iteratively remove default tools sequences via regex
        #      substition list built from longest sequence (first)
        #      to shortest sequence (last)
        #    * build environment tools set with remaining tools
        #    * compute intersection of environment tools and msvc tools sets
        #    * if the intersection is:
        #          empty - no error: default tools and/or no additional msvc tools
        #          not empty - error: user specified one or more msvc tool(s)
        #
        # This will not produce an error or warning when there are no
        # msvc installed instances nor any other recognized compilers
        # and the default environment is needed for a build.  The msvc
        # compiler is forcibly added to the environment tools list when
        # there are no compilers installed on win32. In this case, cl.exe
        # will not be found on the path resulting in a failed build.

        # construct tools string
        tools = _Data.separator.join(tool_list)
        tools_nchar = len(tools)

        debug('msvc default:check tools:nchar=%d, tools=%s', tools_nchar, tools)

        # iteratively remove default tool sequences (longest to shortest)
        if not _Data.default_tools_re_list:
            debug('default_tools_re_list=%s', _Data.default_tools_re_list)
        else:
            re_nchar_min, re_tools_min = _Data.default_tools_re_list[-1]
            if tools_nchar >= re_nchar_min and re_tools_min.search(tools):
                # minimum characters satisfied and minimum pattern exists
                for re_nchar, re_default_tool in _Data.default_tools_re_list:
                    if tools_nchar < re_nchar:
                        # not enough characters for pattern
                        continue
                    tools = re_default_tool.sub('', tools).strip(_Data.separator)
                    tools_nchar = len(tools)
                    debug('msvc default:check tools:nchar=%d, tools=%s', tools_nchar, tools)
                    if tools_nchar < re_nchar_min or not re_tools_min.search(tools):
                        # less than minimum characters or minimum pattern does not exist
                        break

        # construct non-default list(s) tools set
        tools_set = {msvc_tool for msvc_tool in tools.split(_Data.separator) if msvc_tool}

    debug('msvc default:tools=%s', tools_set)
    if not tools_set:
        return None

    # compute intersection of remaining tools set and msvc tools set
    tools_found = _Data.msvc_tools.intersection(tools_set)
    debug('msvc default:tools_exist=%s', tools_found)
    if not tools_found:
        return None

    # construct in same order as tools list
    tools_found_list = []
    seen_tool = set()
    for tool in tool_list:
        if tool not in seen_tool:
            seen_tool.add(tool)
            if tool in tools_found:
                tools_found_list.append(tool)

    # return tool list in order presented
    return tools_found_list

def reset() -> None:
    debug('')
    _Data.reset()

