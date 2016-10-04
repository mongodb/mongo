# Copyright (c) 2010-2012 testtools developers. See LICENSE for details.

__all__ = [
    'safe_hasattr',
    'try_import',
    'try_imports',
    ]

import sys

# Compat - removal announced in 0.9.25.
from extras import (
    safe_hasattr,
    try_import,
    try_imports,
    )


def map_values(function, dictionary):
    """Map ``function`` across the values of ``dictionary``.

    :return: A dict with the same keys as ``dictionary``, where the value
        of each key ``k`` is ``function(dictionary[k])``.
    """
    return dict((k, function(dictionary[k])) for k in dictionary)


def filter_values(function, dictionary):
    """Filter ``dictionary`` by its values using ``function``."""
    return dict((k, v) for k, v in dictionary.items() if function(v))


def dict_subtract(a, b):
    """Return the part of ``a`` that's not in ``b``."""
    return dict((k, a[k]) for k in set(a) - set(b))


def list_subtract(a, b):
    """Return a list ``a`` without the elements of ``b``.

    If a particular value is in ``a`` twice and ``b`` once then the returned
    list then that value will appear once in the returned list.
    """
    a_only = list(a)
    for x in b:
        if x in a_only:
            a_only.remove(x)
    return a_only
