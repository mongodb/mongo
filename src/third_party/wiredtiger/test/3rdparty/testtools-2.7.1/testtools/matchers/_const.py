# Copyright (c) 2016 testtools developers. See LICENSE for details.

__all__ = [
    'Always',
    'Never',
    ]

from ._impl import Mismatch


class _Always:
    """Always matches."""

    def __str__(self):
        return 'Always()'

    def match(self, value):
        return None


def Always():
    """Always match.

    That is::

        self.assertThat(x, Always())

    Will always match and never fail, no matter what ``x`` is. Most useful when
    passed to other higher-order matchers (e.g.
    :py:class:`~testtools.matchers.MatchesListwise`).
    """
    return _Always()


class _Never:
    """Never matches."""

    def __str__(self):
        return 'Never()'

    def match(self, value):
        return Mismatch(
            f'Inevitable mismatch on {value!r}')


def Never():
    """Never match.

    That is::

        self.assertThat(x, Never())

    Will never match and always fail, no matter what ``x`` is. Included for
    completeness with :py:func:`.Always`, but if you find a use for this, let
    us know!
    """
    return _Never()
