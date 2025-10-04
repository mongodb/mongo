# Copyright (c) 2010, 2016 testtools developers. See LICENSE for details.

__all__ = [
    'NeedsTwistedTestCase',
]

from testtools.helpers import try_import
from testtools import TestCase

defer = try_import('twisted.internet.defer')


class NeedsTwistedTestCase(TestCase):

    def setUp(self):
        super().setUp()
        if defer is None:
            self.skipTest("Need Twisted to run")
