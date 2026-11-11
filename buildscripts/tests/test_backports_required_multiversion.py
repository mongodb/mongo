"""Unit tests for backports_required_multiversion.py."""

import textwrap
import unittest

from buildscripts.backports_required_multiversion import InvalidBackportsFile, parse_text

VALID = {
    "empty sections": """
        last-lts:
          all:
          suites:
        """,
    "empty lists": """
        last-continuous:
          all: []
          suites: {}
        """,
    "all sections": """
        last-lts:
          all:
          - test_file: jstests/replsets/server_status_metrics.js
            ticket: SERVER-120318
          suites:
            replica_sets_multiversion:
            - test_file: jstests/replsets/initial_sync_drop_collection.js
              ticket: SERVER-113184
        last-continuous:
          all:
          - test_file: jstests/replsets/server_status_metrics.js
            ticket: SERVER-120318
          suites:
        last-patch:
          all:
          suites:
        """,
    "ticket from another project": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: WT-12345
          suites:
        """,
    "same entry in different lists": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: SERVER-1
          suites:
            replica_sets_multiversion:
            - test_file: jstests/core/foo.js
              ticket: SERVER-1
        """,
}

INVALID = {
    "empty file": "",
    "top level is a list": """
        - last-lts
        """,
    "unknown section": """
        last_lts:
          all:
          suites:
        """,
    "section is a list": """
        last-lts:
        - test_file: jstests/core/foo.js
          ticket: SERVER-1
        """,
    "section missing suites": """
        last-lts:
          all:
        """,
    "section missing all": """
        last-lts:
          suites:
        """,
    "unknown section key": """
        last-lts:
          all:
          suites:
          al:
        """,
    "all is a mapping": """
        last-lts:
          all:
            test_file: jstests/core/foo.js
            ticket: SERVER-1
          suites:
        """,
    "suites is a list": """
        last-lts:
          all:
          suites:
          - test_file: jstests/core/foo.js
            ticket: SERVER-1
        """,
    "entry is a string": """
        last-lts:
          all:
          - jstests/core/foo.js
          suites:
        """,
    "entry missing ticket": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
          suites:
        """,
    "entry unknown key": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: SERVER-1
            tikcet: SERVER-2
          suites:
        """,
    "ticket is a number": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: 1234
          suites:
        """,
    "ticket with underscore": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: SERVER_89698
          suites:
        """,
    "ticket with doubled project": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: SERVER-SERVER-131283
          suites:
        """,
    "lowercase ticket": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: server-1
          suites:
        """,
    "ticket without number": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: SERVER-
          suites:
        """,
    "ticket with trailing newline": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: "SERVER-1\\n"
          suites:
        """,
    "duplicate entry in all": """
        last-lts:
          all:
          - test_file: jstests/core/foo.js
            ticket: SERVER-1
          - test_file: jstests/core/foo.js
            ticket: SERVER-1
          suites:
        """,
    "duplicate entry in a suite": """
        last-lts:
          all:
          suites:
            replica_sets_multiversion:
            - test_file: jstests/core/foo.js
              ticket: SERVER-1
            - test_file: jstests/core/foo.js
              ticket: SERVER-1
        """,
}


class TestParseBackportsRequired(unittest.TestCase):
    def test_valid(self):
        for name, text in VALID.items():
            with self.subTest(name):
                parse_text(textwrap.dedent(text))

    def test_invalid(self):
        for name, text in INVALID.items():
            with self.subTest(name):
                with self.assertRaises(InvalidBackportsFile):
                    parse_text(textwrap.dedent(text))


if __name__ == "__main__":
    unittest.main()
