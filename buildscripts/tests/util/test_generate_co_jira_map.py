"""Unit tests for generate_co_jira_map.py and the co_jira_map lookups."""

import textwrap
import unittest

import buildscripts.util.generate_co_jira_map as under_test
from buildscripts.util.codeowners_utils import Owners

CODEOWNERS = textwrap.dedent(
    """
    # A comment.
    * @10gen/mongo-default-approvers @svc-auto-approve-bot
    /buildscripts/** @10gen/devprod-build someone@mongodb.com
    /src/mongo/db/repl/** @10gen/server-oplog @10gen/server-replication
    """
).strip()

MOTHRA_TEAMS = {
    "10gen/devprod-build": "DevProd Build",
    "10gen/server-oplog": "Replication",
    "10gen/server-replication": "Replication",
    "10gen/platsec-server": "Product Security Server Team",
}


class TestGithubTeamsFromCodeowners(unittest.TestCase):
    def test_only_teams_are_returned(self):
        self.assertEqual(
            under_test.github_teams_from_codeowners(CODEOWNERS),
            {
                "10gen/mongo-default-approvers",
                "10gen/devprod-build",
                "10gen/server-oplog",
                "10gen/server-replication",
            },
        )


class TestBuildMap(unittest.TestCase):
    def test_mothra_team_is_used(self):
        result = under_test.build_map({"10gen/devprod-build"}, MOTHRA_TEAMS, {})
        self.assertEqual(result["10gen/devprod-build"][0], ["DevProd Build"])
        self.assertEqual(result["10gen/devprod-build"][1], "")

    def test_team_missing_from_mothra_is_unmapped(self):
        result = under_test.build_map({"10gen/server-rss"}, MOTHRA_TEAMS, {})
        self.assertEqual(result["10gen/server-rss"][0], [None])
        self.assertIn("Not in mothra", result["10gen/server-rss"][1])

    def test_invalid_jira_option_is_unmapped(self):
        result = under_test.build_map({"10gen/platsec-server"}, MOTHRA_TEAMS, {})
        self.assertEqual(result["10gen/platsec-server"][0], [None])
        self.assertIn("not a valid JIRA option", result["10gen/platsec-server"][1])

    def test_renamed_jira_team_uses_its_new_name(self):
        mothra_teams = {"10gen/query-integration": "Query Integration"}
        result = under_test.build_map({"10gen/query-integration"}, mothra_teams, {})
        self.assertEqual(result["10gen/query-integration"][0], ["DB Integration & Observability"])

    def test_hand_mapped_team_missing_from_mothra_is_kept(self):
        previous = {"10gen/server-rss": ["Replication"]}
        result = under_test.build_map({"10gen/server-rss"}, MOTHRA_TEAMS, previous)
        self.assertEqual(result["10gen/server-rss"][0], ["Replication"])

    def test_teams_only_in_the_previous_map_are_kept(self):
        previous = {"10gen/retired-team": ["Replication"]}
        result = under_test.build_map({"10gen/devprod-build"}, MOTHRA_TEAMS, previous)
        self.assertEqual(sorted(result), ["10gen/devprod-build", "10gen/retired-team"])

    def test_null_previous_mapping_stays_unmapped(self):
        previous = {"10gen/server-root-ownership": [None]}
        result = under_test.build_map(set(), MOTHRA_TEAMS, previous)
        self.assertEqual(result["10gen/server-root-ownership"][0], [None])


class TestRender(unittest.TestCase):
    def test_round_trips_through_yaml(self):
        import yaml

        rendered = under_test.render(
            {
                "10gen/devprod-build": (["DevProd Build"], ""),
                "10gen/server-rss": ([None], " # Not in mothra."),
            }
        )
        self.assertIn("# Not in mothra.", rendered)
        self.assertEqual(
            yaml.safe_load(rendered),
            {"10gen/devprod-build": ["DevProd Build"], "10gen/server-rss": [None]},
        )


class TestOwnersJiraTeamLookup(unittest.TestCase):
    @staticmethod
    def _owners(co_jira_map):
        # Owners reads the real map from disk on construction; bypass that.
        owners = Owners.__new__(Owners)
        owners.co_jira_map = co_jira_map
        return owners

    def test_mapped_codeowner_returns_its_teams(self):
        owners = self._owners({"10gen/devprod-build": ["DevProd Build"]})
        self.assertEqual(
            owners.get_jira_team_from_codeowner("10gen/devprod-build"), ["DevProd Build"]
        )

    def test_null_mapping_returns_no_teams(self):
        owners = self._owners({"10gen/server-rss": [None]})
        self.assertEqual(owners.get_jira_team_from_codeowner("10gen/server-rss"), [])

    def test_unknown_codeowner_raises(self):
        with self.assertRaises(KeyError):
            self._owners({}).get_jira_team_from_codeowner("10gen/nope")


if __name__ == "__main__":
    unittest.main()
