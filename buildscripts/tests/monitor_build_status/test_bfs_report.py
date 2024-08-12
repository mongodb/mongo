import unittest

import buildscripts.monitor_build_status.bfs_report as under_test


evg_projects_info = under_test.EvgProjectsInfo(
    project_to_branch_map={
        "mongodb-mongo-master": "master",
        "mongodb-mongo-master-nightly": "master",
        "sys-perf": "master",
    },
    branch_to_projects_map={
        "master": ["mongodb-mongo-master", "mongodb-mongo-master-nightly", "sys-perf"]
    },
    active_project_names=["mongodb-mongo-master", "mongodb-mongo-master-nightly", "sys-perf"],
    tracking_branches=["master"],
)


class TestBFsReport(unittest.TestCase):
    def test_add_bf_with_multiple_correctness_projects(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["mongodb-mongo-master", "mongodb-mongo-master-nightly"],
            temperature=under_test.BfTemperature.HOT,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(bfs_report.team_reports["team-1"].hot_bfs, {"BF-1"})
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].perf_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_hot_correctness_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["mongodb-mongo-master"],
            temperature=under_test.BfTemperature.HOT,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(bfs_report.team_reports["team-1"].hot_bfs, {"BF-1"})
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].perf_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_cold_correctness_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["mongodb-mongo-master"],
            temperature=under_test.BfTemperature.COLD,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].cold_bfs, {"BF-1"})
        self.assertEqual(len(bfs_report.team_reports["team-1"].perf_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_hot_perf_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["sys-perf"],
            temperature=under_test.BfTemperature.HOT,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].perf_bfs, {"BF-1"})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_cold_perf_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["sys-perf"],
            temperature=under_test.BfTemperature.COLD,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].perf_bfs, {"BF-1"})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_combine_teams(self):
        bfs_report = under_test.BFsReport(
            team_reports={
                "Team 1": under_test.CategorizedBFs(
                    hot_bfs={"HOT-BF-1", "HOT-BF-2"},
                    cold_bfs={"COLD-BF-1", "COLD-BF-2"},
                    perf_bfs={"PERF-BF-1", "PERF-BF-2"},
                ),
                "Team 2": under_test.CategorizedBFs(
                    hot_bfs={"HOT-BF-3", "HOT-BF-4"},
                    cold_bfs={"COLD-BF-3", "COLD-BF-4"},
                    perf_bfs={"PERF-BF-3", "PERF-BF-4"},
                ),
            }
        )

        bfs_report.combine_teams(["Team 1", "Team 2"], "Group 1")

        self.assertEqual(
            bfs_report.team_reports["Group 1"].hot_bfs,
            {"HOT-BF-1", "HOT-BF-2", "HOT-BF-3", "HOT-BF-4"},
        )
        self.assertEqual(
            bfs_report.team_reports["Group 1"].cold_bfs,
            {"COLD-BF-1", "COLD-BF-2", "COLD-BF-3", "COLD-BF-4"},
        )
        self.assertEqual(
            bfs_report.team_reports["Group 1"].perf_bfs,
            {"PERF-BF-1", "PERF-BF-2", "PERF-BF-3", "PERF-BF-4"},
        )
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)
