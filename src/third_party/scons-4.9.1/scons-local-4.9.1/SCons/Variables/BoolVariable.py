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

"""Variable type for true/false Variables.

Usage example::

    opts = Variables()
    opts.Add(BoolVariable('embedded', 'build for an embedded system', False))
    env = Environment(variables=opts)
    if env['embedded']:
        ...
"""

from __future__ import annotations

from typing import Callable

import SCons.Errors

__all__ = ['BoolVariable',]

TRUE_STRINGS = ('y', 'yes', 'true', 't', '1', 'on', 'all')
FALSE_STRINGS = ('n', 'no', 'false', 'f', '0', 'off', 'none')


def _text2bool(val: str | bool) -> bool:
    """Convert boolean-like string to boolean.

    If *val* looks like it expresses a bool-like value, based on
    the :const:`TRUE_STRINGS` and :const:`FALSE_STRINGS` tuples,
    return the appropriate value.

    This is usable as a converter function for SCons Variables.

    Raises:
        ValueError: if *val* cannot be converted to boolean.
    """
    if isinstance(val, bool):
        # mainly for the subst=False case: default might be a bool
        return val
    lval = val.lower()
    if lval in TRUE_STRINGS:
        return True
    if lval in FALSE_STRINGS:
        return False
    # TODO: leave this check to validator?
    raise ValueError(f"Invalid value for boolean variable: {val!r}")


def _validator(key: str, val, env) -> None:
    """Validate that the value of *key* in *env* is a boolean.

    Parameter *val* is not used in the check.

    Usable as a validator function for SCons Variables.

    Raises:
        KeyError: if *key* is not set in *env*
        UserError: if the value of *key* is not ``True`` or ``False``.

    """
    if env[key] not in (True, False):
        msg = f'Invalid value for boolean variable {key!r}: {env[key]}'
        raise SCons.Errors.UserError(msg) from None

# lint: W0622: Redefining built-in 'help' (redefined-builtin)
def BoolVariable(key, help: str, default) -> tuple[str, str, str, Callable, Callable]:
    """Return a tuple describing a boolean SCons Variable.

    The input parameters describe a boolean variable, using a string
    value as described by :const:`TRUE_STRINGS` and :const:`FALSE_STRINGS`.
    Returns a tuple including the correct converter and validator.
    The *help* text will have ``(yes|no)`` automatically appended to
    show the valid values. The result is usable as input to
    :meth:`~SCons.Variables.Variables.Add`.
    """
    help = f'{help} (yes|no)'
    return key, help, default, _validator, _text2bool

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
