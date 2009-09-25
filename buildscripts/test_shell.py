# Copyright 2009 10gen, Inc.
#
# This file is part of MongoDB.
#
# MongoDB is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# MongoDB is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with MongoDB.  If not, see <http://www.gnu.org/licenses/>.

"""Tests for the MongoDB shell.

Right now these mostly just test that the shell handles command line arguments
appropriately.
"""

import unittest
import sys
import subprocess
import os

"""Exit codes for MongoDB."""
BADOPTS = 2
NOCONNECT = 255

"""Path to the mongo shell executable to be tested."""
mongo_path = None

class TestShell(unittest.TestCase):

    def open_mongo(self, args=[]):
        """Get a subprocess.Popen instance of the shell with the given args.
        """
        return subprocess.Popen([mongo_path] + args,
                                stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr = subprocess.PIPE)

    def setUp(self):
        assert mongo_path

    def test_help(self):
        mongo_h = self.open_mongo(["-h"])
        mongo_help = self.open_mongo(["--help"])

        out = mongo_h.communicate()
        self.assertEqual(out, mongo_help.communicate())
        self.assert_("usage:" in out[0])

        self.assertEqual(0, mongo_h.returncode)
        self.assertEqual(0, mongo_help.returncode)

    def test_nodb(self):
        mongo = self.open_mongo([])
        mongo_nodb = self.open_mongo(["--nodb"])

        out = mongo_nodb.communicate()
        self.assert_("MongoDB shell version" in out[0])
        self.assert_("bye" in out[0])
        self.assert_("couldn't connect" not in out[0])
        self.assertEqual(0, mongo_nodb.returncode)

        out = mongo.communicate()
        self.assert_("MongoDB shell version" in out[0])
        self.assert_("bye" not in out[0])
        self.assert_("couldn't connect" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

    def test_eval(self):
        mongo = self.open_mongo(["--nodb", "--eval", "print('hello world');"])
        out = mongo.communicate()
        self.assert_("hello world" in out[0])
        self.assert_("bye" not in out[0])
        self.assertEqual(0, mongo.returncode)

        mongo = self.open_mongo(["--eval"])
        out = mongo.communicate()
        self.assert_("required parameter is missing" in out[0])
        self.assertEqual(BADOPTS, mongo.returncode)

    def test_shell(self):
        mongo = self.open_mongo(["--nodb", "--shell", "--eval", "print('hello world');"])
        out = mongo.communicate()
        self.assert_("hello world" in out[0])
        self.assert_("bye" in out[0]) # the shell started and immediately exited because stdin was empty
        self.assertEqual(0, mongo.returncode)

    def test_host_port(self):
        mongo = self.open_mongo([])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: test" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["--host", "localhost"])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: localhost/test" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["--port", "27018"])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: 127.0.0.1:27018" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["--host", "localhost", "--port", "27018"])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: localhost:27018/test" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["--host"])
        out = mongo.communicate()
        self.assert_("required parameter is missing" in out[0])
        self.assertEqual(BADOPTS, mongo.returncode)

        mongo = self.open_mongo(["--port"])
        out = mongo.communicate()
        self.assert_("required parameter is missing" in out[0])
        self.assertEqual(BADOPTS, mongo.returncode)

    def test_positionals(self):
        dirname = os.path.dirname(__file__)
        test_js = os.path.join(dirname, "testdata/test.js")
        test_txt = os.path.join(dirname, "testdata/test.txt")
        test = os.path.join(dirname, "testdata/test")
        non_exist_js = os.path.join(dirname, "testdata/nonexist.js")
        non_exist_txt = os.path.join(dirname, "testdata/nonexist.txt")

        mongo = self.open_mongo(["--nodb", test_js])
        out = mongo.communicate()
        self.assert_("hello world" in out[0])
        self.assert_("bye" not in out[0])
        self.assertEqual(0, mongo.returncode)

        mongo = self.open_mongo(["--nodb", test_txt])
        out = mongo.communicate()
        self.assert_("foobar" in out[0])
        self.assert_("bye" not in out[0])
        self.assertEqual(0, mongo.returncode)

        mongo = self.open_mongo([test_js, test, test_txt])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: test" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo([test_txt, test, test_js])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: test" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo([test, test_js, test_txt])
        out = mongo.communicate()
        self.assert_("url: " + test in out[0])
        self.assert_("connecting to: " + test in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo([non_exist_js, test, test_txt])
        out = mongo.communicate()
        self.assert_("url: test" in out[0])
        self.assert_("connecting to: test" in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo([non_exist_txt, test_js, test_txt])
        out = mongo.communicate()
        self.assert_("url: " + non_exist_txt in out[0])
        self.assert_("connecting to: " + non_exist_txt in out[0])
        self.assertEqual(NOCONNECT, mongo.returncode)

    def test_multiple_files(self):
        dirname = os.path.dirname(__file__)
        test_js = os.path.join(dirname, "testdata/test.js")
        test_txt = os.path.join(dirname, "testdata/test.txt")

        mongo = self.open_mongo(["--nodb", test_js, test_txt])
        out = mongo.communicate()
        self.assert_("hello world" in out[0])
        self.assert_("foobar" in out[0])
        self.assert_("bye" not in out[0])
        self.assertEqual(0, mongo.returncode)

        mongo = self.open_mongo(["--shell", "--nodb", test_js, test_txt])
        out = mongo.communicate()
        self.assert_("hello world" in out[0])
        self.assert_("foobar" in out[0])
        self.assert_("bye" in out[0])
        self.assertEqual(0, mongo.returncode)

    # just testing that they don't blow up
    def test_username_and_password(self):
        mongo = self.open_mongo(["--username", "mike"])
        out = mongo.communicate()
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["-u", "mike"])
        out = mongo.communicate()
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["--password", "mike"])
        out = mongo.communicate()
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["-p", "mike"])
        out = mongo.communicate()
        self.assertEqual(NOCONNECT, mongo.returncode)

        mongo = self.open_mongo(["--username"])
        out = mongo.communicate()
        self.assert_("required parameter is missing" in out[0])
        self.assertEqual(BADOPTS, mongo.returncode)

        mongo = self.open_mongo(["--password"])
        out = mongo.communicate()
        self.assert_("required parameter is missing" in out[0])
        self.assertEqual(BADOPTS, mongo.returncode)


def run_tests():
    suite = unittest.TestLoader().loadTestsFromTestCase(TestShell)
    unittest.TextTestRunner(verbosity=1).run(suite)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print "must give the path to shell executable to be tested"
        sys.exit()

    mongo_path = sys.argv[1]
    run_tests()
