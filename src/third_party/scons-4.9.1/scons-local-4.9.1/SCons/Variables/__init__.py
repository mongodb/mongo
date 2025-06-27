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

"""Adds user-friendly customizable variables to an SCons build."""

from __future__ import annotations

import os.path
import sys
from contextlib import suppress
from dataclasses import dataclass
from functools import cmp_to_key
from typing import Any, Callable, Sequence

import SCons.Errors
import SCons.Util
import SCons.Warnings

# Note: imports are for the benefit of SCons.Main (and tests); since they
#   are not used here, the "as Foo" form is for checkers.
from .BoolVariable import BoolVariable
from .EnumVariable import EnumVariable
from .ListVariable import ListVariable
from .PackageVariable import PackageVariable
from .PathVariable import PathVariable

__all__ = [
    "Variable",
    "Variables",
    "BoolVariable",
    "EnumVariable",
    "ListVariable",
    "PackageVariable",
    "PathVariable",
]


@dataclass(order=True)
class Variable:
    """A Build Variable."""
    __slots__ = ('key', 'aliases', 'help', 'default', 'validator', 'converter', 'do_subst')
    key: str
    aliases: list[str]
    help: str
    default: Any
    validator: Callable | None
    converter: Callable | None
    do_subst: bool


class Variables:
    """A container for Build Variables.

    Includes a method to populate the variables with values into a
    construction envirionment, and methods to render the help text.

    Note that the pubic API for creating a ``Variables`` object is
    :func:`SCons.Script.Variables`, a kind of factory function, which
    defaults to supplying the contents of :attr:`~SCons.Script.ARGUMENTS`
    as the *args* parameter if it was not otherwise given. That is the
    behavior documented in the manpage for ``Variables`` - and different
    from the default if you instantiate this directly.

    Arguments:
      files: string or list of strings naming variable config scripts
         (default ``None``)
      args: dictionary to override values set from *files*.  (default ``None``)
      is_global: if true, return a global singleton :class:`Variables` object
         instead of a fresh instance. Currently inoperable (default ``False``)

    .. versionchanged:: 4.8.0
       The default for *is_global* changed to ``False`` (the previous
       default ``True`` had no effect due to an implementation error).

    .. deprecated:: 4.8.0
       *is_global* is deprecated.

    .. versionadded:: 4.9.0
       The :attr:`defaulted` attribute now lists those variables which
       were filled in from default values.
    """

    def __init__(
        self,
        files: str | Sequence[str | None] = None,
        args: dict | None = None,
        is_global: bool = False,
    ) -> None:
        self.options: list[Variable] = []
        self.args = args if args is not None else {}
        if not SCons.Util.is_Sequence(files):
            files = [files] if files else []
        self.files: Sequence[str] = files
        self.unknown: dict[str, str] = {}
        self.defaulted: list[str] = []

    def __str__(self) -> str:
        """Provide a way to "print" a :class:`Variables` object."""
        opts = ',\n'.join((f"    {option!s}" for option in self.options))
        return (
            f"Variables(\n  options=[\n{opts}\n  ],\n"
            f"  args={self.args},\n"
            f"  files={self.files},\n"
            f"  unknown={self.unknown},\n"
            f"  defaulted={self.defaulted},\n)"
        )

    # lint: W0622: Redefining built-in 'help'
    def _do_add(
        self,
        key: str | Sequence[str],
        help: str = "",
        default=None,
        validator: Callable | None = None,
        converter: Callable | None = None,
        **kwargs,
    ) -> None:
        """Create a :class:`Variable` and add it to the list.

        This is the internal implementation for :meth:`Add` and
        :meth:`AddVariables`. Not part of the public API.

        .. versionadded:: 4.8.0
              *subst* keyword argument is now recognized.
        """
        # aliases needs to be a list for later concatenation operations
        if SCons.Util.is_Sequence(key):
            name, aliases = key[0], list(key[1:])
        else:
            name, aliases = key, []
        if not name.isidentifier():
            raise SCons.Errors.UserError(f"Illegal Variables key {name!r}")
        do_subst = kwargs.pop("subst", True)
        option = Variable(name, aliases, help, default, validator, converter, do_subst)
        self.options.append(option)

        # options might be added after the 'unknown' dict has been set up:
        # look for and remove the key and all its aliases from that dict
        for alias in option.aliases + [option.key,]:
            if alias in self.unknown:
                del self.unknown[alias]

    def keys(self) -> list:
        """Return the variable names."""
        for option in self.options:
            yield option.key

    def Add(
        self, key: str | Sequence, *args, **kwargs,
    ) -> None:
        """Add a Build Variable.

        Arguments:
          key: the name of the variable, or a 5-tuple (or other sequence).
            If *key* is a tuple, and there are no additional arguments
            except the *help*, *default*, *validator* and *converter*
            keyword arguments, *key* is unpacked into the variable name
            plus the *help*, *default*, *validator* and *converter*
            arguments; if there are additional arguments, the first
            elements of *key* is taken as the variable name, and the
            remainder as aliases.
          args: optional positional arguments, corresponding to the
            *help*, *default*, *validator* and *converter* keyword args.
          kwargs: arbitrary keyword arguments used by the variable itself.

        Keyword Args:
          help: help text for the variable (default: empty string)
          default: default value for variable (default: ``None``)
          validator: function called to validate the value (default: ``None``)
          converter: function to be called to convert the variable's
            value before putting it in the environment. (default: ``None``)
          subst: perform substitution on the value before the converter
            and validator functions (if any) are called (default: ``True``)

        .. versionadded:: 4.8.0
              The *subst* keyword argument is now specially recognized.
        """
        if SCons.Util.is_Sequence(key):
            # If no other positional args (and no fundamental kwargs),
            # unpack key, and pass the kwargs on:
            known_kw = {'help', 'default', 'validator', 'converter'}
            if not args and not known_kw.intersection(kwargs.keys()):
                return self._do_add(*key, **kwargs)

        return self._do_add(key, *args, **kwargs)

    def AddVariables(self, *optlist) -> None:
        """Add Build Variables.

        Each *optlist* element is a sequence of arguments to be passed on
        to the underlying method for adding variables.

        Example::

            opt = Variables()
            opt.AddVariables(
                ('debug', '', 0),
                ('CC', 'The C compiler'),
                ('VALIDATE', 'An option for testing validation', 'notset', validator, None),
            )
        """
        for opt in optlist:
            self._do_add(*opt)

    def Update(self, env, args: dict | None = None) -> None:
        """Update an environment with the Build Variables.

        This is where the work of adding variables to the environment
        happens, The input sources saved at init time are scanned for
        variables to add, though if *args* is passed, then it is used
        instead of the saved one. If any variable description set up
        a callback for a validator and/or converter, those are called.
        Variables from the input sources which do not match a variable
        description in this object are ignored for purposes of adding
        to *env*, but are saved in the :attr:`unknown` dict attribute.
        Variables which are set in *env* from the default in a variable
        description and not from the input sources are saved in the
        :attr:`defaulted` list attribute.

        Args:
            env: the environment to update.
            args: a dictionary of keys and values to update in *env*.
               If omitted, uses the saved :attr:`args`
        """
        # first pull in the defaults, except any which are None.
        values = {opt.key: opt.default for opt in self.options if opt.default is not None}
        self.defaulted = list(values)

        # next set the values specified in any saved-variables script(s)
        for filename in self.files:
            if os.path.exists(filename):
                # lint: W0622: Redefining built-in 'dir'
                dir = os.path.split(os.path.abspath(filename))[0]
                if dir:
                    sys.path.insert(0, dir)
                try:
                    values['__name__'] = filename
                    with open(filename) as f:
                        contents = f.read()
                    exec(contents, {}, values)
                finally:
                    if dir:
                        del sys.path[0]
                    del values['__name__']

        # set the values specified on the command line
        if args is None:
            args = self.args

        for arg, value in args.items():
            added = False
            for option in self.options:
                if arg in option.aliases + [option.key,]:
                    values[option.key] = value
                    with suppress(ValueError):
                        self.defaulted.remove(option.key)
                    added = True
            if not added:
                self.unknown[arg] = value

        # put the variables in the environment
        # (don't copy over variables that are not declared as options)
        #
        # Nitpicking: in OO terms, this method increases coupling as its
        #   main work is to update a different object (env), rather than
        #   the object it's bound to (although it does update self, too).
        #   It's tricky to decouple because the algorithm counts on directly
        #   setting a var in *env* first so it can call env.subst() on it
        #   to transform it.

        for option in self.options:
            try:
                env[option.key] = values[option.key]
            except KeyError:
                pass

        # apply converters
        for option in self.options:
            if option.converter and option.key in values:
                if option.do_subst:
                    value = env.subst('${%s}' % option.key)
                else:
                    value = env[option.key]
                try:
                    try:
                        env[option.key] = option.converter(value)
                    except TypeError:
                        env[option.key] = option.converter(value, env)
                except ValueError as exc:
                    # We usually want the converter not to fail and leave
                    # that to the validator, but in case, handle it.
                    msg = f'Error converting option: {option.key!r}\n{exc}'
                    raise SCons.Errors.UserError(msg) from exc

        # apply validators
        for option in self.options:
            if option.validator and option.key in values:
                if option.do_subst:
                    val = env[option.key]
                    if not SCons.Util.is_String(val):
                        # issue #4585: a _ListVariable should not be further
                        #    substituted, breaks on values with spaces.
                        value = val
                    else:
                        value = env.subst('${%s}' % option.key)
                else:
                    value = env[option.key]
                option.validator(option.key, value, env)

    def UnknownVariables(self) -> dict:
        """Return dict of unknown variables.

        Identifies variables that were not recognized in this object.
        """
        return self.unknown

    def Save(self, filename, env) -> None:
        """Save the variables to a script.

        Saves all the variables which have non-default settings
        to the given file as Python expressions.  This script can
        then be used to load the variables for a subsequent run.
        This can be used to create a build variable "cache" or
        capture different configurations for selection.

        Args:
            filename: Name of the file to save into
            env: the environment to get the option values from
        """
        # Create the file and write out the header
        try:
            # TODO: issue #816 use Node to access saved-variables file?
            with open(filename, 'w') as fh:
                # Make an assignment in the file for each option
                # within the environment that was assigned a value
                # other than the default. We don't want to save the
                # ones set to default: in case the SConscript settings
                # change you would then pick up old defaults.
                for option in self.options:
                    try:
                        value = env[option.key]
                        try:
                            prepare = value.prepare_to_store
                        except AttributeError:
                            try:
                                eval(repr(value))
                            except KeyboardInterrupt:
                                raise
                            except Exception:
                                # Convert stuff that has a repr() that
                                # cannot be evaluated into a string
                                value = SCons.Util.to_String(value)
                        else:
                            value = prepare()

                        defaultVal = env.subst(SCons.Util.to_String(option.default))
                        if option.converter:
                            try:
                                defaultVal = option.converter(defaultVal)
                            except TypeError:
                                defaultVal = option.converter(defaultVal, env)

                        if str(env.subst(f'${option.key}')) != str(defaultVal):
                            fh.write(f'{option.key} = {value!r}\n')
                    except KeyError:
                        pass
        except OSError as exc:
            msg = f'Error writing options to file: {filename}\n{exc}'
            raise SCons.Errors.UserError(msg) from exc

    def GenerateHelpText(self, env, sort: bool | Callable = False) -> str:
        """Generate the help text for the Variables object.

        Args:
            env: an environment that is used to get the current values
                of the variables.
            sort: Either a comparison function used for sorting
                (must take two arguments and return ``-1``, ``0`` or ``1``)
                or a boolean to indicate if it should be sorted.
        """
        # TODO this interface was designed when Python's sorted() took an
        #   optional comparison function (pre-3.0). Since it no longer does,
        #   we use functools.cmp_to_key() since can't really change the
        #   documented meaning of the "sort" argument. Maybe someday?
        if callable(sort):
            options = sorted(self.options, key=cmp_to_key(lambda x, y: sort(x.key, y.key)))
        elif sort is True:
            options = sorted(self.options)
        else:
            options = self.options

        def format_opt(opt, self=self, env=env) -> str:
            if opt.key in env:
                actual = env.subst(f'${opt.key}')
            else:
                actual = None
            return self.FormatVariableHelpText(
                env, opt.key, opt.help, opt.default, actual, opt.aliases
            )
        return ''.join(_f for _f in map(format_opt, options) if _f)

    fmt = '\n%s: %s\n    default: %s\n    actual: %s\n'
    aliasfmt = '\n%s: %s\n    default: %s\n    actual: %s\n    aliases: %s\n'

    # lint: W0622: Redefining built-in 'help'
    def FormatVariableHelpText(
        self,
        env,
        key: str,
        help: str,
        default,
        actual,
        aliases: list[str | None] = None,
    ) -> str:
        """Format the help text for a single variable.

        The caller is responsible for obtaining all the values,
        although now the :class:`Variable` class is more publicly exposed,
        this method could easily do most of that work - however
        that would change the existing published API.
        """
        if aliases is None:
            aliases = []
        # Don't display the key name itself as an alias.
        aliases = [a for a in aliases if a != key]
        if aliases:
            return self.aliasfmt % (key, help, default, actual, aliases)
        return self.fmt % (key, help, default, actual)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
