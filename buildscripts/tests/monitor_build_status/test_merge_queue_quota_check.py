"""Tests for the merge-queue quota gate (SERVER-138460)."""

import itertools
import json
import pathlib
import tempfile
import unittest
from unittest.mock import MagicMock

import buildscripts.monitor_build_status.merge_queue_quota_check as under_test

# (ticket, team, over_quota, exempt, linked, expected)
CASES = [
    (None, "Query", True, False, frozenset(), under_test.Verdict.UNRESOLVED),  # no ticket
    ("SERVER-1", None, True, False, frozenset(), under_test.Verdict.UNRESOLVED),  # no team
    ("SERVER-1", "Query", None, False, frozenset(), under_test.Verdict.UNRESOLVED),  # no quota
    ("SERVER-1", "Query", False, False, frozenset(), under_test.Verdict.PASS),  # under quota
    ("SERVER-1", "Query", True, True, frozenset(), under_test.Verdict.PASS),  # exempt
    ("SERVER-1", "Query", True, False, frozenset({"BF"}), under_test.Verdict.PASS),  # fixes a BF
    ("SERVER-1", "Query", True, False, frozenset({"SECBUG"}), under_test.Verdict.PASS),
    ("SERVER-1", "Query", True, False, frozenset({"SERVER"}), under_test.Verdict.BLOCK),
    ("SERVER-1", "Query", True, False, frozenset(), under_test.Verdict.BLOCK),
    # ordering: an exemption must not rescue an unresolvable ticket
    (None, "Query", True, True, frozenset({"BF"}), under_test.Verdict.UNRESOLVED),
]


class TestDecide(unittest.TestCase):
    def test_decision_table(self):
        for ticket, team, over_quota, exempt, linked, expected in CASES:
            with self.subTest(ticket=ticket, team=team, over_quota=over_quota):
                facts = under_test.Facts(
                    ticket=ticket,
                    team=team,
                    over_quota=over_quota,
                    exempt=exempt,
                    linked_projects=linked,
                )
                self.assertIs(under_test.decide(facts).verdict, expected)

    def test_decision_is_total(self):
        """Every combination must land on a verdict — the table ends in BLOCK."""
        space = itertools.product(
            [None, "SERVER-1", "SERVER-XXX"],
            [None, "Query"],
            [True, False, None],
            [True, False],
            [frozenset(), frozenset({"BF"})],
        )
        for ticket, team, over_quota, exempt, linked in space:
            facts = under_test.Facts(
                ticket=ticket,
                team=team,
                over_quota=over_quota,
                exempt=exempt,
                linked_projects=linked,
            )
            self.assertIn(under_test.decide(facts).verdict, set(under_test.Verdict))


class TestPullRequestFromSubject(unittest.TestCase):
    def test_merge_queue_subject(self):
        self.assertEqual(
            under_test.pull_request_from_subject(
                "GitHub Merge Queue: SERVER-138312 Require wheels (#65719)"
            ),
            65719,
        )

    def test_takes_the_first_match_only(self):
        self.assertEqual(
            under_test.pull_request_from_subject("GitHub Merge Queue: SERVER-1 x (#100) (3dfea65)"),
            100,
        )


class TestTicketFromSubject(unittest.TestCase):
    def test_leading_ticket(self):
        self.assertEqual(
            under_test.ticket_from_subject("GitHub Merge Queue: SERVER-138312 Require wheels (#1)"),
            "SERVER-138312",
        )

    def test_referenced_ticket_is_ignored(self):
        """A subject naming a second ticket in passing must not decide the gate."""
        self.assertEqual(
            under_test.ticket_from_subject("SERVER-132793: Update TODO listed in SERVER-130659"),
            "SERVER-132793",
        )

    def test_placeholder_has_no_digits(self):
        # placeholders read as "no ticket", so the gate fails open on them
        self.assertIsNone(under_test.ticket_from_subject("SERVER-XXX: placeholder"))

    def test_wiredtiger_import_has_no_ticket(self):
        """A WT sync subject carries no ticket, so the gate fails open on it.

        This is what exempts WT imports — no special case needed. Their body
        lists WT tickets, which is why only the subject is read.
        """
        self.assertIsNone(
            under_test.ticket_from_subject(
                "GitHub Merge Queue: Import wiredtiger: 4324a8b4 from branch "
                "mongodb-master (#66955)"
            )
        )


class TestLinkedProjects(unittest.TestCase):
    """The BF/AF/security exemption rests entirely on this reading the links."""

    @staticmethod
    def issue(*links):
        return MagicMock(fields=MagicMock(issuelinks=list(links)))

    def test_outward_link(self):
        issue = self.issue(MagicMock(outwardIssue=MagicMock(key="BF-47170"), inwardIssue=None))
        self.assertEqual(under_test.linked_projects(issue), {"BF"})

    def test_inward_link(self):
        """A link is stored on whichever side it was created from."""
        issue = self.issue(MagicMock(outwardIssue=None, inwardIssue=MagicMock(key="SECBUG-6225")))
        self.assertEqual(under_test.linked_projects(issue), {"SECBUG"})

    def test_both_directions_and_deduplication(self):
        issue = self.issue(
            MagicMock(outwardIssue=MagicMock(key="BF-1"), inwardIssue=None),
            MagicMock(outwardIssue=None, inwardIssue=MagicMock(key="BF-2")),
            MagicMock(outwardIssue=MagicMock(key="SERVER-139220"), inwardIssue=None),
        )
        self.assertEqual(under_test.linked_projects(issue), {"BF", "SERVER"})

    def test_no_links(self):
        self.assertEqual(under_test.linked_projects(self.issue()), frozenset())

    def test_issue_without_a_links_field(self):
        """The Jira library omits the field entirely when there are no links."""
        self.assertEqual(
            under_test.linked_projects(MagicMock(fields=MagicMock(spec=[]))), frozenset()
        )


class TestAssigneeEmail(unittest.TestCase):
    def test_reads_the_assignee(self):
        issue = MagicMock(fields=MagicMock(assignee=MagicMock(emailAddress="a.b@mongodb.com")))
        self.assertEqual(under_test.assignee_email(issue), "a.b@mongodb.com")

    def test_unassigned(self):
        self.assertIsNone(under_test.assignee_email(MagicMock(fields=MagicMock(assignee=None))))


class TestVerdictRecord(unittest.TestCase):
    def test_written_when_the_gate_fails_open(self):
        """The upload step expects the file; a missing one would fail the task."""
        with tempfile.TemporaryDirectory() as tmp:
            path = str(pathlib.Path(tmp) / "verdict.json")
            under_test.write_verdict(path, "SERVER-1 x (#5)", under_test.FAILED, None)
            record = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
        self.assertEqual(record["pr"], 5)
        self.assertEqual(record["verdict"], "unresolved")
        self.assertIsNone(record["team"])


class TestApprovedEmails(unittest.TestCase):
    def test_parses_the_list(self):
        """The shipped list is validated on first run; here we test the loader."""
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "exceptions.yml"
            path.write_text(
                "exceptions:\n"
                "  - email: Someone@MongoDB.com\n"
                "    reason: Example\n"
                "  - email: other@mongodb.com\n"
                "    reason: Example\n",
                encoding="utf-8",
            )
            self.assertEqual(
                under_test.approved_emails(str(path)),
                {"someone@mongodb.com", "other@mongodb.com"},  # lower-cased
            )

    def test_malformed_entries_are_ignored(self):
        """A bad row must not take the whole gate down."""
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "exceptions.yml"
            path.write_text(
                "exceptions:\n"
                "  - email: good@mongodb.com\n"
                "  - reason: no email here\n"
                "  - not-a-mapping\n"
                "  - email:\n",
                encoding="utf-8",
            )
            self.assertEqual(under_test.approved_emails(str(path)), {"good@mongodb.com"})


if __name__ == "__main__":
    unittest.main()
