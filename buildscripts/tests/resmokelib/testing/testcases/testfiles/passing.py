import unittest


class PassingTest(unittest.TestCase):
    """Dummy test case that always passes."""

    def test_pass(self):
        """A test that always passes."""
        self.assertTrue(True)
