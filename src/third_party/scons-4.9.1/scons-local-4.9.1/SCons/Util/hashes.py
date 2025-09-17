# SPDX-License-Identifier: MIT
#
# Copyright The SCons Foundation

"""
SCons hash utility routines.

Routines for working with content and signature hashes.
"""

from __future__ import annotations

import functools
import hashlib
import sys

from .sctypes import to_bytes


# Default hash function and format. SCons-internal.
DEFAULT_HASH_FORMATS = ['md5', 'sha1', 'sha256']
ALLOWED_HASH_FORMATS = []
_HASH_FUNCTION = None
_HASH_FORMAT = None


def _attempt_init_of_python_3_9_hash_object(hash_function_object, sys_used=sys):
    """Initialize hash function with non-security indicator.

    In Python 3.9 and onwards, :mod:`hashlib` constructors accept a
    keyword argument *usedforsecurity*, which, if set to ``False``,
    lets us continue to use algorithms that have been deprecated either
    by FIPS or by Python itself, as the MD5 algorithm SCons prefers is
    not being used for security purposes as much as a short, 32 char
    hash that is resistant to accidental collisions.

    In prior versions of python, :mod:`hashlib` returns a native function
    wrapper, which errors out when it's queried for the optional
    parameter, so this function wraps that call.

    It can still throw a ValueError if the initialization fails due to
    FIPS compliance issues, but that is assumed to be the responsibility
    of the caller.
    """
    if hash_function_object is None:
        return None

    # https://stackoverflow.com/a/11887885 details how to check versions
    # with the "packaging" library. However, for our purposes, checking
    # the version is greater than or equal to 3.9 is good enough, as the API
    # is guaranteed to have support for the 'usedforsecurity' flag in 3.9. See
    # https://docs.python.org/3/library/hashlib.html#:~:text=usedforsecurity
    # for the version support notes.
    if (sys_used.version_info.major > 3) or (
        sys_used.version_info.major == 3 and sys_used.version_info.minor >= 9
    ):
        return hash_function_object(usedforsecurity=False)

    # Note that this can throw a ValueError in FIPS-enabled versions of
    # Linux prior to 3.9. The OpenSSL hashlib will throw on first init here,
    # but it is assumed to be responsibility of the caller to diagnose the
    # ValueError & potentially display the error to screen.
    return hash_function_object()


def _set_allowed_viable_default_hashes(hashlib_used, sys_used=sys) -> None:
    """Check if the default hash algorithms can be called.

    This util class is sometimes called prior to setting the
    user-selected hash algorithm, meaning that on FIPS-compliant systems
    the library would default-initialize MD5 and throw an exception in
    set_hash_format. A common case is using the SConf options, which can
    run prior to main, and thus ignore the options.hash_format variable.

    This function checks the DEFAULT_HASH_FORMATS and sets the
    ALLOWED_HASH_FORMATS to only the ones that can be called. In Python
    >= 3.9 this will always default to MD5 as in Python 3.9 there is an
    optional attribute "usedforsecurity" set for the method.

    Throws if no allowed hash formats are detected.
    """
    global ALLOWED_HASH_FORMATS
    _last_error = None
    # note: if you call this method repeatedly, example using timeout,
    # this is needed. Otherwise it keeps appending valid formats to the string.
    ALLOWED_HASH_FORMATS = []

    for test_algorithm in DEFAULT_HASH_FORMATS:
        _test_hash = getattr(hashlib_used, test_algorithm, None)
        # we know hashlib claims to support it... check to see if we can call it.
        if _test_hash is not None:
            # The hashing library will throw an exception on initialization
            # in FIPS mode, meaning if we call the default algorithm returned
            # with no parameters, it'll throw if it's a bad algorithm,
            # otherwise it will append it to the known good formats.
            try:
                _attempt_init_of_python_3_9_hash_object(_test_hash, sys_used)
                ALLOWED_HASH_FORMATS.append(test_algorithm)
            except ValueError as e:
                _last_error = e
                continue

    if len(ALLOWED_HASH_FORMATS) == 0:
        from SCons.Errors import (  # pylint: disable=import-outside-toplevel
            SConsEnvironmentError,
        )

        # chain the exception thrown with the most recent error from hashlib.
        raise SConsEnvironmentError(
            'No usable hash algorithms found.'
            'Most recent error from hashlib attached in trace.'
        ) from _last_error


_set_allowed_viable_default_hashes(hashlib)


def get_hash_format():
    """Retrieves the hash format or ``None`` if not overridden.

    A return value of ``None``
    does not guarantee that MD5 is being used; instead, it means that the
    default precedence order documented in :func:`SCons.Util.set_hash_format`
    is respected.
    """
    return _HASH_FORMAT


def _attempt_get_hash_function(hash_name, hashlib_used=hashlib, sys_used=sys):
    """Wrapper used to try to initialize a hash function given.

    If successful, returns the name of the hash function back to the user.

    Otherwise returns None.
    """
    try:
        _fetch_hash = getattr(hashlib_used, hash_name, None)
        if _fetch_hash is None:
            return None
        _attempt_init_of_python_3_9_hash_object(_fetch_hash, sys_used)
        return hash_name
    except ValueError:
        # If attempt_init_of_python_3_9 throws, this is typically due to FIPS
        # being enabled. However, if we get to this point, the viable hash
        # function check has either been bypassed or otherwise failed to
        # properly restrict the user to only the supported functions.
        # As such throw the UserError as an internal assertion-like error.
        return None


def set_hash_format(hash_format, hashlib_used=hashlib, sys_used=sys):
    """Sets the default hash format used by SCons.

    If `hash_format` is ``None`` or
    an empty string, the default is determined by this function.

    Currently the default behavior is to use the first available format of
    the following options: MD5, SHA1, SHA256.
    """
    global _HASH_FORMAT, _HASH_FUNCTION

    _HASH_FORMAT = hash_format
    if hash_format:
        hash_format_lower = hash_format.lower()
        if hash_format_lower not in ALLOWED_HASH_FORMATS:
            from SCons.Errors import (  # pylint: disable=import-outside-toplevel
                UserError,
            )

            # User can select something not supported by their OS but
            # normally supported by SCons, example, selecting MD5 in an
            # OS with FIPS-mode turned on. Therefore we first check if
            # SCons supports it, and then if their local OS supports it.
            if hash_format_lower in DEFAULT_HASH_FORMATS:
                raise UserError(
                    'While hash format "%s" is supported by SCons, the '
                    'local system indicates only the following hash '
                    'formats are supported by the hashlib library: %s'
                    % (hash_format_lower, ', '.join(ALLOWED_HASH_FORMATS))
                )

            # The hash format isn't supported by SCons in any case.
            # Warn the user, and if we detect that SCons supports more
            # algorithms than their local system supports,
            # warn the user about that too.
            if ALLOWED_HASH_FORMATS == DEFAULT_HASH_FORMATS:
                raise UserError(
                    'Hash format "%s" is not supported by SCons. Only '
                    'the following hash formats are supported: %s'
                    % (hash_format_lower, ', '.join(ALLOWED_HASH_FORMATS))
                )

            raise UserError(
                'Hash format "%s" is not supported by SCons. '
                'SCons supports more hash formats than your local system '
                'is reporting; SCons supports: %s. Your local system only '
                'supports: %s'
                % (
                    hash_format_lower,
                    ', '.join(DEFAULT_HASH_FORMATS),
                    ', '.join(ALLOWED_HASH_FORMATS),
                )
            )

        # This is not expected to fail. If this fails it means the
        # set_allowed_viable_default_hashes function did not throw,
        # or when it threw, the exception was caught and ignored, or
        # the global ALLOWED_HASH_FORMATS was changed by an external user.
        _HASH_FUNCTION = _attempt_get_hash_function(
            hash_format_lower, hashlib_used, sys_used
        )

        if _HASH_FUNCTION is None:
            from SCons.Errors import (  # pylint: disable=import-outside-toplevel
                UserError,
            )

            raise UserError(
                f'Hash format "{hash_format_lower}" is not available in your '
                'Python interpreter. Expected to be supported algorithm by '
                'set_allowed_viable_default_hashes. Assertion error in SCons.'
            )
    else:
        # Set the default hash format based on what is available, defaulting
        # to the first supported hash algorithm (usually md5) for backwards
        # compatibility. In FIPS-compliant systems this usually defaults to
        # SHA1, unless that too has been disabled.
        for choice in ALLOWED_HASH_FORMATS:
            _HASH_FUNCTION = _attempt_get_hash_function(choice, hashlib_used, sys_used)

            if _HASH_FUNCTION is not None:
                break
        else:
            # This is not expected to happen in practice.
            from SCons.Errors import (  # pylint: disable=import-outside-toplevel
                UserError,
            )

            raise UserError(
                'Your Python interpreter does not have MD5, SHA1, or SHA256. '
                'SCons requires at least one. Expected to support one or more '
                'during set_allowed_viable_default_hashes.'
            )


# Ensure that this is initialized in case either:
#    1. This code is running in a unit test.
#    2. This code is running in a consumer that does hash operations while
#       SConscript files are being loaded.
set_hash_format(None)


def get_current_hash_algorithm_used():
    """Returns the current hash algorithm name used.

    Where the python version >= 3.9, this is expected to return md5.
    If python's version is <= 3.8, this returns md5 on non-FIPS-mode platforms, and
    sha1 or sha256 on FIPS-mode Linux platforms.

    This function is primarily useful for testing, where one expects a value to be
    one of N distinct hashes, and therefore the test needs to know which hash to select.
    """
    return _HASH_FUNCTION


def _get_hash_object(hash_format, hashlib_used=hashlib, sys_used=sys):
    """Allocates a hash object using the requested hash format.

    Args:
        hash_format: Hash format to use.

    Returns:
        hashlib object.
    """
    if hash_format is None:
        if _HASH_FUNCTION is None:
            from SCons.Errors import (  # pylint: disable=import-outside-toplevel
                UserError,
            )

            raise UserError(
                'There is no default hash function. Did you call '
                'a hashing function before SCons was initialized?'
            )
        return _attempt_init_of_python_3_9_hash_object(
            getattr(hashlib_used, _HASH_FUNCTION, None), sys_used
        )

    if not hasattr(hashlib, hash_format):
        from SCons.Errors import UserError  # pylint: disable=import-outside-toplevel

        raise UserError(
            f'Hash format "{hash_format}" is not available in your Python interpreter.'
        )

    return _attempt_init_of_python_3_9_hash_object(
        getattr(hashlib, hash_format), sys_used
    )


def hash_signature(s, hash_format=None):
    """
    Generate hash signature of a string

    Args:
        s: either string or bytes. Normally should be bytes
        hash_format: Specify to override default hash format

    Returns:
        String of hex digits representing the signature
    """
    m = _get_hash_object(hash_format)
    try:
        m.update(to_bytes(s))
    except TypeError:
        m.update(to_bytes(str(s)))

    return m.hexdigest()


def hash_file_signature(fname, chunksize: int=65536, hash_format=None):
    """
    Generate the md5 signature of a file

    Args:
        fname: file to hash
        chunksize: chunk size to read
        hash_format: Specify to override default hash format

    Returns:
        String of Hex digits representing the signature
    """

    m = _get_hash_object(hash_format)
    with open(fname, "rb") as f:
        while True:
            blck = f.read(chunksize)
            if not blck:
                break
            m.update(to_bytes(blck))
        # TODO: can use this when base is Python 3.8+
        # while (blk := f.read(chunksize)) != b'':
        #     m.update(to_bytes(blk))

    return m.hexdigest()


def hash_collect(signatures, hash_format=None):
    """
    Collects a list of signatures into an aggregate signature.

    Args:
        signatures: a list of signatures
        hash_format: Specify to override default hash format

    Returns:
        the aggregate signature
    """

    if len(signatures) == 1:
        return signatures[0]

    return hash_signature(', '.join(signatures), hash_format)


_MD5_WARNING_SHOWN = False


def _show_md5_warning(function_name) -> None:
    """Shows a deprecation warning for various MD5 functions."""

    global _MD5_WARNING_SHOWN

    if not _MD5_WARNING_SHOWN:
        import SCons.Warnings  # pylint: disable=import-outside-toplevel

        SCons.Warnings.warn(
            SCons.Warnings.DeprecatedWarning,
            f"Function {function_name} is deprecated",
        )
        _MD5_WARNING_SHOWN = True


def MD5signature(s):
    """Deprecated. Use :func:`hash_signature` instead."""

    _show_md5_warning("MD5signature")
    return hash_signature(s)


def MD5filesignature(fname, chunksize: int=65536):
    """Deprecated. Use :func:`hash_file_signature` instead."""

    _show_md5_warning("MD5filesignature")
    return hash_file_signature(fname, chunksize)


def MD5collect(signatures):
    """Deprecated. Use :func:`hash_collect` instead."""

    _show_md5_warning("MD5collect")
    return hash_collect(signatures)


# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
