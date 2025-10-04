# Copyright (c) 2010-2012 testtools developers. See LICENSE for details.

import sys


def try_import(name, alternative=None, error_callback=None):
    """Attempt to import a module, with a fallback.

    Attempt to import ``name``.  If it fails, return ``alternative``.  When
    supporting multiple versions of Python or optional dependencies, it is
    useful to be able to try to import a module.

    :param name: The name of the object to import, e.g. ``os.path`` or
        ``os.path.join``.
    :param alternative: The value to return if no module can be imported.
        Defaults to None.
    :param error_callback: If non-None, a callable that is passed the
        ImportError when the module cannot be loaded.
    """
    module_segments = name.split('.')
    last_error = None
    remainder = []

    # module_name will be what successfully imports. We cannot walk from the
    # __import__ result because in import loops (A imports A.B, which imports
    # C, which calls try_import("A.B")) A.B will not yet be set.
    while module_segments:
        module_name = '.'.join(module_segments)
        try:
            __import__(module_name)
        except ImportError:
            last_error = sys.exc_info()[1]
            remainder.append(module_segments.pop())
            continue
        else:
            break
    else:
        if last_error is not None and error_callback is not None:
            error_callback(last_error)
        return alternative

    module = sys.modules[module_name]
    nonexistent = object()
    for segment in reversed(remainder):
        module = getattr(module, segment, nonexistent)
        if module is nonexistent:
            if last_error is not None and error_callback is not None:
                error_callback(last_error)
            return alternative

    return module


def map_values(function, dictionary):
    """Map ``function`` across the values of ``dictionary``.

    :return: A dict with the same keys as ``dictionary``, where the value
        of each key ``k`` is ``function(dictionary[k])``.
    """
    return {k: function(dictionary[k]) for k in dictionary}


def filter_values(function, dictionary):
    """Filter ``dictionary`` by its values using ``function``."""
    return {k: v for k, v in dictionary.items() if function(v)}


def dict_subtract(a, b):
    """Return the part of ``a`` that's not in ``b``."""
    return {k: a[k] for k in set(a) - set(b)}


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
