# Copyright (c) 2011 testtools developers. See LICENSE for details.

"""Compatibility helpers that are valid syntax in Python 2.x.

Only add things here if they *only* work in Python 2.x or are Python 2
alternatives to things that *only* work in Python 3.x.
"""

__all__ = [
    'reraise',
    ]


def reraise(exc_class, exc_obj, exc_tb, _marker=object()):
    """Re-raise an exception received from sys.exc_info() or similar."""
    raise exc_class, exc_obj, exc_tb

