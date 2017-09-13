# Copyright (c) 2010-2012 extras developers. See LICENSE for details.

"""Extensions to the Python standard library."""

import sys

__all__ = [
    'safe_hasattr',
    'try_import',
    'try_imports',
    ]

# same format as sys.version_info: "A tuple containing the five components of
# the version number: major, minor, micro, releaselevel, and serial. All
# values except releaselevel are integers; the release level is 'alpha',
# 'beta', 'candidate', or 'final'. The version_info value corresponding to the
# Python version 2.0 is (2, 0, 0, 'final', 0)."  Additionally we use a
# releaselevel of 'dev' for unreleased under-development code.
#
# If the releaselevel is 'alpha' then the major/minor/micro components are not
# established at this point, and setup.py will use a version of next-$(revno).
# If the releaselevel is 'final', then the tarball will be major.minor.micro.
# Otherwise it is major.minor.micro~$(revno).

__version__ = (0, 0, 3, 'final', 0)


def try_import(name, alternative=None, error_callback=None):
    """Attempt to import ``name``.  If it fails, return ``alternative``.

    When supporting multiple versions of Python or optional dependencies, it
    is useful to be able to try to import a module.

    :param name: The name of the object to import, e.g. ``os.path`` or
        ``os.path.join``.
    :param alternative: The value to return if no module can be imported.
        Defaults to None.
    :param error_callback: If non-None, a callable that is passed the ImportError
        when the module cannot be loaded.
    """
    module_segments = name.split('.')
    last_error = None
    while module_segments:
        module_name = '.'.join(module_segments)
        try:
            module = __import__(module_name)
        except ImportError:
            last_error = sys.exc_info()[1]
            module_segments.pop()
            continue
        else:
            break
    else:
        if last_error is not None and error_callback is not None:
            error_callback(last_error)
        return alternative
    nonexistent = object()
    for segment in name.split('.')[1:]:
        module = getattr(module, segment, nonexistent)
        if module is nonexistent:
            if last_error is not None and error_callback is not None:
                error_callback(last_error)
            return alternative
    return module


_RAISE_EXCEPTION = object()
def try_imports(module_names, alternative=_RAISE_EXCEPTION, error_callback=None):
    """Attempt to import modules.

    Tries to import the first module in ``module_names``.  If it can be
    imported, we return it.  If not, we go on to the second module and try
    that.  The process continues until we run out of modules to try.  If none
    of the modules can be imported, either raise an exception or return the
    provided ``alternative`` value.

    :param module_names: A sequence of module names to try to import.
    :param alternative: The value to return if no module can be imported.
        If unspecified, we raise an ImportError.
    :param error_callback: If None, called with the ImportError for *each*
        module that fails to load.
    :raises ImportError: If none of the modules can be imported and no
        alternative value was specified.
    """
    module_names = list(module_names)
    for module_name in module_names:
        module = try_import(module_name, error_callback=error_callback)
        if module:
            return module
    if alternative is _RAISE_EXCEPTION:
        raise ImportError(
            "Could not import any of: %s" % ', '.join(module_names))
    return alternative


def safe_hasattr(obj, attr, _marker=object()):
    """Does 'obj' have an attribute 'attr'?

    Use this rather than built-in hasattr, as the built-in swallows exceptions
    in some versions of Python and behaves unpredictably with respect to
    properties.
    """
    return getattr(obj, attr, _marker) is not _marker


