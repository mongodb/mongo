import unittest


class TestAssert(unittest.TestCase):
    def test_pass(self):
        true = True
        self.assertTrue(true)


if __name__ == "__main__":
    unittest.main()
