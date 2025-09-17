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
Internal method dispatcher for Microsoft Visual C/C++.

MSVC modules can register their module (register_modulename) and individual
classes (register_class) with the method dispatcher during initialization. MSVC
modules tend to be registered immediately after the Dispatcher import near the
top of the file. Methods in the MSVC modules can be invoked indirectly without
having to hard-code the method calls effectively decoupling the upstream module
with the downstream modules:

The reset method dispatches calls to all registered objects with a reset method
and/or a _reset method. The reset methods are used to restore data structures
to their initial state for testing purposes. Typically, this involves clearing
cached values.

The verify method dispatches calls to all registered objects with a verify
method and/or a _verify method. The verify methods are used to check that
initialized data structures distributed across multiple modules are internally
consistent.  An exception is raised when a verification constraint violation
is detected.  Typically, this verifies that initialized dictionaries support
all of the requisite keys as new versions are added.
"""

import sys

from ..common import (
    debug,
)

_refs = []


def register_modulename(modname) -> None:
    module = sys.modules[modname]
    _refs.append(module)


def register_class(ref) -> None:
    _refs.append(ref)


def reset() -> None:
    debug('')
    for ref in _refs:
        for method in ['reset', '_reset']:
            if not hasattr(ref, method) or not callable(getattr(ref, method, None)):
                continue
            debug('call %s.%s()', ref.__name__, method)
            func = getattr(ref, method)
            func()


def verify() -> None:
    debug('')
    for ref in _refs:
        for method in ['verify', '_verify']:
            if not hasattr(ref, method) or not callable(getattr(ref, method, None)):
                continue
            debug('call %s.%s()', ref.__name__, method)
            func = getattr(ref, method)
            func()
