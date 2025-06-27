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

"""Variable type for enumeration Variables.

Enumeration variables allow selection of one from a specified set of values.

Usage example::

    opts = Variables()
    opts.Add(
        EnumVariable(
            'debug',
            help='debug output and symbols',
            default='no',
            allowed_values=('yes', 'no', 'full'),
            map={},
            ignorecase=2,
        )
    )
    env = Environment(variables=opts)
    if env['debug'] == 'full':
        ...
"""

from __future__ import annotations

from typing import Callable

import SCons.Errors

__all__ = ['EnumVariable',]


def _validator(key, val, env, vals) -> None:
    """Validate that val is in vals.

    Usable as the base for :class:`EnumVariable` validators.
    """
    if val not in vals:
        msg = (
            f"Invalid value for enum variable {key!r}: {val!r}. "
            f"Valid values are: {vals}"
        )
        raise SCons.Errors.UserError(msg) from None


# lint: W0622: Redefining built-in 'help' (redefined-builtin)
# lint:  W0622: Redefining built-in 'map' (redefined-builtin)
def EnumVariable(
    key,
    help: str,
    default: str,
    allowed_values: list[str],
    map: dict | None = None,
    ignorecase: int = 0,
) -> tuple[str, str, str, Callable, Callable]:
    """Return a tuple describing an enumaration SCons Variable.

    An Enum Variable is an abstraction that allows choosing one
    value from a provided list of possibilities (*allowed_values*).
    The value of *ignorecase* defines the behavior of the
    validator and converter: if ``0``, the validator/converter are
    case-sensitive; if ``1``, the validator/converter are case-insensitive;
    if ``2``, the validator/converter are case-insensitive and the
    converted value will always be lower-case.

    Arguments:
       key: the name of the variable.
       default: default value, passed directly through to the return tuple.
       help: descriptive part of the help text,
          will have the allowed values automatically appended.
       allowed_values: the values for the choice.
       map: optional dictionary which may be used for converting the
          input value into canonical values (e.g. for aliases).
       ignorecase: defines the behavior of the validator and converter.

    Returns:
       A tuple including an appropriate converter and validator.
       The result is usable as input to :meth:`~SCons.Variables.Variables.Add`.
       and :meth:`~SCons.Variables.Variables.AddVariables`.
    """
    # these are all inner functions so they can access EnumVariable locals.
    def validator_rcase(key, val, env):
        """Case-respecting validator."""
        return _validator(key, val, env, allowed_values)

    def validator_icase(key, val, env):
        """Case-ignoring validator."""
        return _validator(key, val.lower(), env, allowed_values)

    def converter_rcase(val):
        """Case-respecting converter."""
        return map.get(val, val)

    def converter_icase(val):
        """Case-ignoring converter."""
        return map.get(val.lower(), val)

    def converter_lcase(val):
        """Case-lowering converter."""
        return map.get(val.lower(), val).lower()

    if map is None:
        map = {}
    help = f"{help} ({'|'.join(allowed_values)})"

    # define validator
    if ignorecase:
        validator = validator_icase
    else:
        validator = validator_rcase

    # define converter
    if ignorecase == 2:
        converter = converter_lcase
    elif ignorecase == 1:
        converter = converter_icase
    else:
        converter = converter_rcase

    return key, help, default, validator, converter

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
