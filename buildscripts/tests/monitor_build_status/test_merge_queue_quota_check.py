"""Tests for the merge-queue quota gate (SERVER-138460)."""

import contextlib
import io
import itertools
import json
import pathlib
import tempfile
import unittest
from unittest.mock import MagicMock, patch

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
    """The fix-work exemption rests entirely on this reading the links."""

    @staticmethod
    def issue(*links):
        return MagicMock(fields=MagicMock(issuelinks=list(links)))

    @staticmethod
    def link(link_type, key, side):
        """A link as the Jira library hands it back."""
        kind = MagicMock()
        kind.name = link_type
        far = "inwardIssue" if side == "outwardIssue" else "outwardIssue"
        return MagicMock(type=kind, **{side: MagicMock(key=key), far: None})

    def test_a_fixes_link_counts(self):
        issue = self.issue(self.link("Fix", "BF-47170", "outwardIssue"))
        self.assertEqual(under_test.linked_projects(issue), {"BF"})

    def test_a_depends_link_counts_on_the_inward_side(self):
        """SERVER is depended on by the BF, so the BF sits on the inward side."""
        issue = self.issue(self.link("Depends", "SECBUG-6225", "inwardIssue"))
        self.assertEqual(under_test.linked_projects(issue), {"SECBUG"})

    def test_related_does_not_count(self):
        """It is a bookmark. Most BFs carry one and it says nothing about a fix."""
        issue = self.issue(self.link("Related", "BF-1", "outwardIssue"))
        self.assertEqual(under_test.linked_projects(issue), frozenset())

    def test_problem_incident_does_not_count(self):
        """It marks the change that caused the failure, not the fix."""
        issue = self.issue(self.link("Problem/Incident", "BF-1", "inwardIssue"))
        self.assertEqual(under_test.linked_projects(issue), frozenset())

    def test_depends_the_other_way_round_does_not_count(self):
        """SERVER depending on the BF is not SERVER fixing it."""
        issue = self.issue(self.link("Depends", "BF-1", "outwardIssue"))
        self.assertEqual(under_test.linked_projects(issue), frozenset())

    def test_fixes_the_other_way_round_does_not_count(self):
        issue = self.issue(self.link("Fix", "BF-1", "inwardIssue"))
        self.assertEqual(under_test.linked_projects(issue), frozenset())

    def test_mixes_and_deduplicates(self):
        issue = self.issue(
            self.link("Depends", "BF-1", "inwardIssue"),
            self.link("Depends", "BF-2", "inwardIssue"),
            self.link("Fix", "AF-9", "outwardIssue"),
            self.link("Related", "BF-3", "outwardIssue"),
            self.link("Fix", "SERVER-139220", "outwardIssue"),
        )
        self.assertEqual(under_test.linked_projects(issue), {"BF", "AF", "SERVER"})

    def test_a_link_without_a_type_is_skipped(self):
        """Ignoring it beats raising, which would fail the whole run open."""
        link = MagicMock(spec=["outwardIssue", "inwardIssue"])
        link.outwardIssue = MagicMock(key="BF-1")
        link.inwardIssue = None
        self.assertEqual(under_test.linked_projects(self.issue(link)), frozenset())

    def test_no_links(self):
        self.assertEqual(under_test.linked_projects(self.issue()), frozenset())

    def test_evidence_keeps_the_type_of_every_link(self):
        """The log has to say why a link did not count, not just that it did not."""
        issue = self.issue(
            self.link("Related", "BF-1", "outwardIssue"),
            self.link("Fix", "AF-2", "outwardIssue"),
        )
        self.assertEqual(
            under_test.links_seen(issue),
            (
                under_test.Link(key="BF-1", link_type="Related", counted=False),
                under_test.Link(key="AF-2", link_type="Fix", counted=True),
            ),
        )

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


class TestMainFailsOpen(unittest.TestCase):
    """The gate must never fail a merge-queue build. A crash stops every merge."""

    def setUp(self):
        for name, value in (
            ("CodeLockdownConfig", MagicMock()),
            ("JiraClient", MagicMock()),
            ("JiraAuth", MagicMock()),
            ("approved_emails", frozenset()),
            ("enable_logging", MagicMock()),
        ):
            patcher = (
                patch.object(under_test, name, return_value=value)
                if name == "approved_emails"
                else patch.object(under_test, name, value)
            )
            patcher.start()
            self.addCleanup(patcher.stop)

    @staticmethod
    def run_main(**kwargs):
        """main() prints the audit; keep it out of the test output."""
        with contextlib.redirect_stdout(io.StringIO()):
            return under_test.main(**kwargs)

    def test_evaluation_raising_returns_zero(self):
        with patch.object(under_test, "evaluate", side_effect=RuntimeError("jira is down")):
            self.assertEqual(self.run_main(commit_message="SERVER-1 x (#1)"), 0)

    def test_the_audit_print_raising_returns_zero(self):
        """A diagnostic must not be able to stop merges."""
        with patch.object(under_test, "evaluate", return_value=(under_test.FAILED, None)):
            with patch.object(under_test, "print_audit", side_effect=RuntimeError("boom")):
                self.assertEqual(self.run_main(commit_message="SERVER-1 x (#1)"), 0)

    def test_the_record_is_written_even_when_evaluation_raises(self):
        """The upload step expects the file; a missing one fails the task."""
        with tempfile.TemporaryDirectory() as tmp:
            path = str(pathlib.Path(tmp) / "verdict.json")
            with patch.object(under_test, "evaluate", side_effect=RuntimeError("jira is down")):
                self.assertEqual(
                    self.run_main(commit_message="SERVER-1 x (#1)", verdict_file=path), 0
                )
            record = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
        self.assertEqual(record["verdict"], "unresolved")
        self.assertEqual(record["rule"], "lookup-failed")


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
