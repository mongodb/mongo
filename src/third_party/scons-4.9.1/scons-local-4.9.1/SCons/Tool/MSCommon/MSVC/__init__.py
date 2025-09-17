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
Functions for Microsoft Visual C/C++.

The _reset method is used to restore MSVC module data structures to their
initial state for testing purposes.

The _verify method is used as a sanity check that MSVC module data structures
are internally consistent.

Currently:
* _reset is invoked from reset_installed_vcs in the vc module.
* _verify is invoked from the last line in the vc module.
"""

from . import Exceptions  # noqa: F401

from . import Config  # noqa: F401
from . import Util  # noqa: F401
from . import Registry  # noqa: F401
from . import Kind  # noqa: F401
from . import SetupEnvDefault  # noqa: F401
from . import Policy  # noqa: F401
from . import WinSDK  # noqa: F401
from . import ScriptArguments  # noqa: F401

from . import Dispatcher as _Dispatcher

def _reset() -> None:
    _Dispatcher.reset()

def _verify() -> None:
    _Dispatcher.verify()

