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

"""Variable type for List Variables.

A list variable allows selecting one or more from a supplied set of
allowable values, as well as from an optional mapping of alternate names
(such as aliases and abbreviations) and the special names ``'all'`` and
``'none'``.  Specified values are converted during processing into values
only from the allowable values set.

Usage example::

    list_of_libs = Split('x11 gl qt ical')

    opts = Variables()
    opts.Add(
        ListVariable(
            'shared',
            help='libraries to build as shared libraries',
            default='all',
            elems=list_of_libs,
        )
    )
    env = Environment(variables=opts)
    for lib in list_of_libs:
        if lib in env['shared']:
            env.SharedObject(...)
        else:
            env.Object(...)
"""

# Known Bug: This should behave like a Set-Type, but does not really,
# since elements can occur twice.

from __future__ import annotations

import collections
import functools
from typing import Callable

import SCons.Util

__all__ = ['ListVariable',]


class _ListVariable(collections.UserList):
    """Internal class holding the data for a List Variable.

    This is normally not directly instantiated, rather the ListVariable
    converter callback "converts" string input (or the default value
    if none) into an instance and stores it.

    Args:
       initlist: the list of actual values given.
       allowedElems: the list of allowable values.
    """

    def __init__(
        self, initlist: list | None = None, allowedElems: list | None = None
    ) -> None:
        if initlist is None:
            initlist = []
        if allowedElems is None:
            allowedElems = []
        super().__init__([_f for _f in initlist if _f])
        # TODO: why sorted? don't we want to display in the order user gave?
        self.allowedElems = sorted(allowedElems)

    def __cmp__(self, other):
        return NotImplemented

    def __eq__(self, other):
        return NotImplemented

    def __ge__(self, other):
        return NotImplemented

    def __gt__(self, other):
        return NotImplemented

    def __le__(self, other):
        return NotImplemented

    def __lt__(self, other):
        return NotImplemented

    def __str__(self) -> str:
        if not self.data:
            return 'none'
        self.data.sort()
        if self.data == self.allowedElems:
            return 'all'
        return ','.join(self)

    def prepare_to_store(self):
        return str(self)

def _converter(val, allowedElems, mapdict) -> _ListVariable:
    """Callback to convert list variables into a suitable form.

    The arguments *allowedElems* and *mapdict* are non-standard
    for a :class:`Variables` converter: the lambda in the
    :func:`ListVariable` function arranges for us to be called correctly.

    Incoming values ``all`` and ``none`` are recognized and converted
    into their expanded form.
    """
    if val == 'none':
        val = []
    elif val == 'all':
        val = allowedElems
    else:
        val = [_f for _f in val.split(',') if _f]
        val = [mapdict.get(v, v) for v in val]
    return _ListVariable(val, allowedElems)


def _validator(key, val, env) -> None:
    """Callback to validate supplied value(s) for a ListVariable.

    Validation means "is *val* in the allowed list"? *val* has
    been subject to substitution before the validator is called. The
    converter created a :class:`_ListVariable` container which is stored
    in *env* after it runs; this includes the allowable elements list.
    Substitution makes a string made out of the values (only),
    so we need to fish the allowed elements list out of the environment
    to complete the validation.

    Note that since 18b45e456, whether ``subst`` has been
    called is conditional on the value of the *subst* argument to
    :meth:`~SCons.Variables.Variables.Add`, so we have to account for
    possible different types of *val*.

    Raises:
       UserError: if validation failed.

    .. versionadded:: 4.8.0
       ``_validator`` split off from :func:`_converter` with an additional
       check  for whether *val* has been substituted before the call.
    """
    allowedElems = env[key].allowedElems
    if isinstance(val, _ListVariable):  # not substituted, use .data
        notAllowed = [v for v in val.data if v not in allowedElems]
    else:  # presumably a string
        notAllowed = [v for v in val.split() if v not in allowedElems]
    if notAllowed:
        # Converter only synthesized 'all' and 'none', they are never
        # in the allowed list, so we need to add those to the error message
        # (as is done for the help msg).
        valid = ','.join(allowedElems + ['all', 'none'])
        msg = (
            f"Invalid value(s) for variable {key!r}: {','.join(notAllowed)!r}. "
            f"Valid values are: {valid}"
        )
        raise SCons.Errors.UserError(msg) from None


# lint: W0622: Redefining built-in 'help' (redefined-builtin)
# lint: W0622: Redefining built-in 'map' (redefined-builtin)
def ListVariable(
    key,
    help: str,
    default: str | list[str],
    names: list[str],
    map: dict | None = None,
    validator: Callable | None = None,
) -> tuple[str, str, str, Callable, Callable]:
    """Return a tuple describing a list variable.

    A List Variable is an abstraction that allows choosing one or more
    values from a provided list of possibilities (*names). The special terms
    ``all`` and ``none`` are also provided to help make the selection.

    Arguments:
       key: the name of the list variable.
       help: the basic help message.  Will have text appended indicating
          the allowed values (not including any extra names from *map*).
       default: the default value(s) for the list variable. Can be given
          as string (use commas to -separated multiple values), or as a list
          of strings.  ``all`` or ``none`` are allowed as *default*.
          A must-specify ListVariable can be simulated by giving a value
          that is not part of *names*, which will cause validation to fail
          if the variable is not given in the input sources.
       names: the values to choose from. Must be a list of strings.
       map: optional dictionary to map alternative names to the ones in
          *names*, providing a form of alias. The converter will make
          the replacement, names from *map* are not stored and will
          not appear in the help message.
       validator: optional callback to validate supplied values.
          The default validator is used if not specified.

    Returns:
       A tuple including the correct converter and validator.  The
       result is usable as input to :meth:`~SCons.Variables.Variables.Add`.

    .. versionchanged:: 4.8.0
       The validation step was split from the converter to allow for
       custom validators.  The *validator* keyword argument was added.
    """
    if map is None:
        map = {}
    if validator is None:
        validator = _validator
    names_str = f"allowed names: {' '.join(names)}"
    if SCons.Util.is_List(default):
        default = ','.join(default)
    help = '\n    '.join(
        (help, '(all|none|comma-separated list of names)', names_str))
    converter = functools.partial(_converter, allowedElems=names, mapdict=map)
    return key, help, default, validator, converter

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
