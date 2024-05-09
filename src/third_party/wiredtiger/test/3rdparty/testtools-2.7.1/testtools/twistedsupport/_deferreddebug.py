# Copyright (c) testtools developers. See LICENSE for details.
#
# TODO: Move this to testtools.twistedsupport. See testing-cabal/testtools#202.

from fixtures import Fixture, MonkeyPatch


class DebugTwisted(Fixture):
    """Set debug options for Twisted."""

    def __init__(self, debug=True):
        super().__init__()
        self._debug_setting = debug

    def _setUp(self):
        self.useFixture(
            MonkeyPatch('twisted.internet.defer.Deferred.debug',
                        self._debug_setting))
        self.useFixture(
            MonkeyPatch('twisted.internet.base.DelayedCall.debug',
                        self._debug_setting))
