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

"""The SCons Warnings framework.

Enables issuing warnings in situations where it is useful to alert
the user of a condition that does not warrant raising an exception
that could terminate the program.

A new warning class should inherit (perhaps indirectly) from one of
two base classes: :exc:`SConsWarning` or :exc:`WarningOnByDefault`,
which are the same except warnings derived from the latter will start
out in an enabled state. Enabled warnings cause a message to be
printed when called, disabled warnings are silent.

There is also a hierarchy for indicating deprecations and future
changes: for these, derive from :exc:`DeprecatedWarning`,
:exc:`MandatoryDeprecatedWarning`, :exc:`FutureDeprecatedWarning`
or :exc:`FutureReservedVariableWarning`.

Whether or not to display warnings, beyond those that are on by
default, is controlled through the command line (``--warn``) or
through ``SetOption('warn')``.  The names used there use a different
naming style than the warning class names. :func:`process_warn_strings`
converts the names before enabling/disabling.

The behavior of issuing only a message (for "enabled" warnings) can
be toggled to raising an exception instead by calling the
:func:`warningAsException` function.

For new/removed warnings, the manpage needs to be kept in sync.
Any warning class defined here is accepted, but we don't want to make
people have to dig around to find the names.  Warnings do not have to
be defined in this file, though it is preferred: those defined elsewhere
cannot use the enable/disable functionality unless they monkeypatch the
warning into this module's namespace.

You issue a warning, either in SCons code or in a build project's
SConscripts, by calling the :func:`warn` function defined in this module.
Raising directly with an instance of a warning class bypasses the
framework and it will behave like an ordinary exception.
"""

from __future__ import annotations

import sys
from typing import Callable, Sequence

import SCons.Errors

# _enabled is a list of 2-tuples with a warning class object and a
# boolean (True if that warning is enabled). Initialized in SCons/Main.py.
_enabled = []

# If False, just emit the msg for an enabled warning; else raise exception
_warningAsException: bool = False

# Function to emit the warning. Initialized by SCons/Main.py for regular use;
# the unit test will set to a capturing version for testing.
_warningOut: Callable | None = None


class SConsWarning(SCons.Errors.UserError):
    """Base class for all SCons warnings."""

class WarningOnByDefault(SConsWarning):
    """Base class for SCons warnings that are enabled by default."""

SConsWarningOnByDefault = WarningOnByDefault  # transition to new name


class LinkWarning(WarningOnByDefault):
    """Base class for linker warnings."""

# NOTE:  If you add a new warning class here, add it to the man page, too!

# General warnings

class CacheVersionWarning(WarningOnByDefault):
    """The derived-file cache directory has an out of date config."""

class CacheWriteErrorWarning(SConsWarning):
    """Problems writing a derived file to the cache."""

class CacheCleanupErrorWarning(SConsWarning):
    """Problems removing retrieved target prior to rebuilding."""

class CorruptSConsignWarning(WarningOnByDefault):
    """Problems decoding the contents of the sconsign database."""

class DependencyWarning(SConsWarning):
    """A scanner identified a dependency but did not add it."""

class DevelopmentVersionWarning(WarningOnByDefault):
    """Use of a deprecated feature."""

class DuplicateEnvironmentWarning(WarningOnByDefault):
    """A target appears in more than one consenv with identical actions.

    A duplicate target with different rules cannot be built;
    with the same rule it can, but this could indicate a problem in
    the build configuration.
    """

class FortranCxxMixWarning(LinkWarning):
    """Fortran and C++ objects appear together in a link line.

    Some compilers support this, others do not.
    """

class FutureReservedVariableWarning(WarningOnByDefault):
    """Setting a variable marked to become reserved in a future release."""

class MisleadingKeywordsWarning(WarningOnByDefault):
    """Use of possibly misspelled kwargs in Builder calls."""

class MissingSConscriptWarning(WarningOnByDefault):
    """The script specified in an SConscript() call was not found.

    TODO: this is now an error, so no need for a warning. Left in for
    a while in case anyone is using, remove eventually.

    Manpage entry removed in 4.6.0.
    """

class NoObjectCountWarning(WarningOnByDefault):
    """Object counting (debug mode) could not be enabled."""

class NoParallelSupportWarning(WarningOnByDefault):
    """Fell back to single-threaded build, as no thread support found."""

class ReservedVariableWarning(WarningOnByDefault):
    """Attempt to set reserved construction variable names."""

class StackSizeWarning(WarningOnByDefault):
    """Requested thread stack size could not be set."""

class TargetNotBuiltWarning(SConsWarning): # TODO: should go to OnByDefault
    """A target build indicated success but the file is not found."""

class VisualCMissingWarning(WarningOnByDefault):
    """Requested MSVC version not found and policy is to not fail."""

class VisualVersionMismatch(WarningOnByDefault):
    """``MSVC_VERSION`` and ``MSVS_VERSION`` do not match.

    Note ``MSVS_VERSION`` is deprecated, use ``MSVC_VERSION``.
    """

class VisualStudioMissingWarning(SConsWarning):  # TODO: unused
    pass


# Deprecation warnings

class FutureDeprecatedWarning(SConsWarning):
    """Base class for features that will become deprecated in a future release."""

class DeprecatedWarning(SConsWarning):
    """Base class for deprecated features, will be removed in future."""

class MandatoryDeprecatedWarning(DeprecatedWarning):
    """Base class for deprecated features where warning cannot be disabled."""


# Special case; base always stays DeprecatedWarning
class PythonVersionWarning(DeprecatedWarning):
    """SCons was run with a deprecated Python version."""

class DeprecatedOptionsWarning(MandatoryDeprecatedWarning):
    """Options that are deprecated."""

class DeprecatedDebugOptionsWarning(MandatoryDeprecatedWarning):
    """Option-arguments to --debug that are deprecated."""

class ToolQtDeprecatedWarning(DeprecatedWarning):  # TODO: unused
    pass


def suppressWarningClass(clazz) -> None:
    """Suppresses all warnings of type *clazz* or derived from *clazz*."""
    _enabled.insert(0, (clazz, False))

def enableWarningClass(clazz) -> None:
    """Enables all warnings of type *clazz* or derived from *clazz*."""
    _enabled.insert(0, (clazz, True))

def warningAsException(flag: bool = True) -> bool:
    """Sets global :data:`_warningAsExeption` flag.

    If true, any enabled warning will cause an exception to be raised.

    Args:
        flag: new value for warnings-as-exceptions.

    Returns:
        The previous value.
    """
    global _warningAsException  # pylint: disable=global-statement
    old = _warningAsException
    _warningAsException = flag
    return old

def warn(clazz, *args) -> None:
    """Issue a warning, accounting for SCons rules.

    Check if warnings for this class are enabled.  If warnings are treated
    as exceptions, raise exception.  Use the global warning emitter
    :data:`_warningOut`, which allows selecting different ways of
    presenting a traceback (see Script/Main.py).
    """
    warning = clazz(args)
    for cls, flag in _enabled:
        if isinstance(warning, cls):
            if flag:
                if _warningAsException:
                    raise warning

                if _warningOut is not None:
                    _warningOut(warning)
            break

def process_warn_strings(arguments: Sequence[str]) -> None:
    """Process requests to enable/disable warnings.

    The requests come from the option-argument string passed to the
    ``--warn`` command line option or as the value passed to the
    ``SetOption`` function with a first argument of ``warn``;


    The arguments are expected to be as documented in the SCons manual
    page for the ``--warn`` option, in the style ``some-type``,
    which is converted here to a camel-case name like ``SomeTypeWarning``,
    to try to match the warning classes defined here, which are then
    passed to :func:`enableWarningClass` or :func:`suppressWarningClass`.

    For example, a string``"deprecated"`` enables the
    :exc:`DeprecatedWarning` class, while a string``"no-dependency"``
    disables the :exc:`DependencyWarning` class.

    As a special case, the string ``"all"`` disables all warnings and
    a the string ``"no-all"`` disables all warnings.
    """
    def _classmunge(s: str) -> str:
        """Convert a warning argument to SConsCase.

        The result is CamelCase, except "Scons" is changed to "SCons"
        """
        s = s.replace("-", " ").title().replace(" ", "")
        return s.replace("Scons", "SCons")

    for arg in arguments:
        enable = True
        if arg.startswith("no-") and arg not in (
            "no-object-count",
            "no-parallel-support",
        ):
            enable = False
            arg = arg[len("no-") :]
        if arg == 'all':
            class_name = "SConsWarning"
        else:
            class_name = _classmunge(arg) + 'Warning'
        try:
            clazz = globals()[class_name]
        except KeyError:
            sys.stderr.write(f"No warning type: {arg!r}\n")
        else:
            if enable:
                enableWarningClass(clazz)
            elif issubclass(clazz, MandatoryDeprecatedWarning):
                sys.stderr.write(f"Can not disable mandataory warning: {arg!r}\n")
            else:
                suppressWarningClass(clazz)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
