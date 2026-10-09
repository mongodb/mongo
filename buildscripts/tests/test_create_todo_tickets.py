"""Unit tests for create_todo_tickets.py."""

import textwrap
import unittest
from unittest import mock

from jira import JIRAError

import buildscripts.create_todo_tickets as under_test

CODEOWNERS = textwrap.dedent(
    """
    * @10gen/mongo-default-approvers @svc-auto-approve-bot
    /buildscripts/** @10gen/devprod-build @svc-auto-approve-bot
    /src/mongo/db/query/** @10gen/query-optimization @svc-auto-approve-bot
    /src/mongo/db/repl/** @10gen/server-replication-reviewers @svc-auto-approve-bot
    /src/mongo/db/repl/*oplog* @10gen/server-oplog @svc-auto-approve-bot
    /src/mongo/db/repl/oplog.cpp @10gen/server-oplog @10gen/query @svc-auto-approve-bot
    """
).strip()

CO_JIRA_MAP = {
    "10gen/devprod-build": ["Build"],
    "10gen/query": ["Query Execution", "Query Integration", "Query Optimization"],
    "10gen/query-optimization": ["Query Optimization"],
    "10gen/server-replication": ["Replication"],
    "10gen/server-root-ownership": None,
}


class TestTeamResolver(unittest.TestCase):
    def setUp(self):
        self.resolver = under_test.TeamResolver(CODEOWNERS, CO_JIRA_MAP)

    def test_directly_mapped_team(self):
        self.assertEqual(self.resolver.jira_teams("buildscripts/create_todo_tickets.py"), ["Build"])

    def test_most_specific_pattern_wins(self):
        self.assertEqual(
            self.resolver.jira_teams("src/mongo/db/query/query_planner.cpp"), ["Query Optimization"]
        )

    def test_unmapped_subteam_falls_back_to_parent_team(self):
        # `server-replication-reviewers` has no mapping of its own.
        self.assertEqual(self.resolver.jira_teams("src/mongo/db/repl/rs_sync.cpp"), ["Replication"])

    def test_multiple_owners_are_unioned_in_order(self):
        # `server-oplog` has no mapping and no mapped ancestor, so it drops out.
        self.assertEqual(
            self.resolver.jira_teams("src/mongo/db/repl/oplog.cpp"),
            ["Query Execution", "Query Integration", "Query Optimization"],
        )

    def test_unmappable_rule_widens_to_the_next_matching_rule(self):
        # The most specific rule names only `server-oplog`, which maps to nothing,
        # so the broader `src/mongo/db/repl/**` rule decides the team.
        self.assertEqual(
            self.resolver.jira_teams("src/mongo/db/repl/oplog_fetcher.cpp"), ["Replication"]
        )

    def test_unowned_file_has_no_teams(self):
        self.assertEqual(self.resolver.jira_teams("README.md"), [])

    def test_null_mapping_is_not_a_team(self):
        self.assertEqual(
            self.resolver._jira_teams_for_codeowner("@10gen/server-root-ownership"), []
        )

    def test_non_team_owners_are_ignored(self):
        resolver = under_test.TeamResolver("* someone@mongodb.com\n", CO_JIRA_MAP)
        self.assertEqual(resolver.jira_teams("buildscripts/foo.py"), [])


class TestResolveTeams(unittest.TestCase):
    def setUp(self):
        self.resolver = under_test.TeamResolver(CODEOWNERS, CO_JIRA_MAP)

    @staticmethod
    def _occurrence(file_path):
        return under_test.TodoOccurrence(
            issue_key="SERVER-1", file_path=file_path, line_number=1, line="// TODO SERVER-1"
        )

    def test_teams_of_all_files_are_combined(self):
        occurrences = [
            self._occurrence("buildscripts/create_todo_tickets.py"),
            self._occurrence("src/mongo/db/query/query_planner.cpp"),
            self._occurrence("buildscripts/other.py"),
        ]
        self.assertEqual(
            under_test.resolve_teams(self.resolver, occurrences), ["Build", "Query Optimization"]
        )

    def test_unowned_files_fall_back_to_triage(self):
        self.assertEqual(
            under_test.resolve_teams(self.resolver, [self._occurrence("README.md")]),
            [under_test.FALLBACK_TEAM],
        )


class TestFindTodos(unittest.TestCase):
    @staticmethod
    def _find(contents):
        return under_test.find_todos(textwrap.dedent(contents).strip().splitlines(), "file.py")

    def test_todo_with_a_ticket_is_found(self):
        todos = self._find(
            """
            some code
            # TODO SERVER-1234: remove this
            more code
            """
        )
        self.assertEqual(len(todos), 1)
        self.assertEqual(todos[0].issue_key, "SERVER-1234")
        self.assertEqual(todos[0].file_path, "file.py")
        self.assertEqual(todos[0].line_number, 2)
        self.assertEqual(todos[0].line, "# TODO SERVER-1234: remove this")

    def test_todo_without_a_ticket_is_ignored(self):
        self.assertEqual(self._find("# TODO clean this up"), [])

    def test_ticket_without_a_todo_is_ignored(self):
        self.assertEqual(self._find("# See SERVER-1234 for details"), [])

    def test_multiple_todos_are_all_found(self):
        todos = self._find(
            """
            # todo WT-1: one
            # TODO REP-2: two
            """
        )
        self.assertEqual([todo.issue_key for todo in todos], ["WT-1", "REP-2"])

    def test_issue_key_is_normalized_to_upper_case(self):
        todos = self._find(
            """
            # TODO server-1234: lower
            # TODO Server-1234: mixed
            # TODO rep-2: lower rep
            """
        )
        self.assertEqual(
            [todo.issue_key for todo in todos], ["SERVER-1234", "SERVER-1234", "REP-2"]
        )

    def test_get_issue_key_from_line_is_case_insensitive(self):
        self.assertEqual(under_test.get_issue_key_from_line("// todo wt-17958"), "WT-17958")
        self.assertIsNone(under_test.get_issue_key_from_line("// todo nothing here"))


class TestConstructDescription(unittest.TestCase):
    def test_description_lists_every_occurrence(self):
        occurrences = [
            under_test.TodoOccurrence("SERVER-1", "src/b.cpp", 20, "// TODO SERVER-1"),
            under_test.TodoOccurrence("SERVER-1", "src/a.cpp", 10, "// TODO SERVER-1"),
        ]
        description = under_test.construct_description("SERVER-1", occurrences)
        self.assertIn("* src/a.cpp:10", description)
        self.assertIn("* src/b.cpp:20", description)
        # Sorted by path, so a.cpp comes first.
        self.assertLess(description.index("src/a.cpp"), description.index("src/b.cpp"))
        self.assertIn("10gen/mongo/search?q=SERVER-1", description)

    def test_rep_tickets_point_at_the_mongosync_repo(self):
        description = under_test.construct_description(
            "REP-1", [under_test.TodoOccurrence("REP-1", "a.go", 1, "// TODO REP-1")]
        )
        self.assertIn("10gen/mongosync/search?q=REP-1", description)


if __name__ == "__main__":
    unittest.main()


class TestProcessTodos(unittest.TestCase):
    def _jira(self, create_issue_side_effect):
        jira = mock.MagicMock()
        jira.issue.return_value.fields.status = "Resolved"
        jira.search_issues.return_value = None
        jira.create_issue.side_effect = create_issue_side_effect
        return jira

    def _todos(self, *keys):
        return {
            key: [under_test.TodoOccurrence(key, "src/a.cpp", 1, f"// TODO {key}")] for key in keys
        }

    def test_failed_ticket_does_not_stop_remaining_tickets(self):
        bad_request = JIRAError(status_code=400, text="bad field")
        new_issue = mock.MagicMock(key="SERVER-9")
        jira = self._jira([bad_request, new_issue])
        resolver = mock.MagicMock()
        resolver.jira_teams.return_value = ["Server Triage"]

        failed = under_test.process_todos(jira, resolver, self._todos("SERVER-1", "SERVER-2"))

        self.assertEqual(failed, ["SERVER-1"])
        self.assertEqual(jira.create_issue.call_count, 2)
        jira.create_issue_link.assert_called_once()

    def test_no_failures_returns_empty_list(self):
        jira = self._jira([mock.MagicMock(key="SERVER-9")])
        resolver = mock.MagicMock()
        resolver.jira_teams.return_value = ["Server Triage"]

        self.assertEqual(under_test.process_todos(jira, resolver, self._todos("SERVER-1")), [])
