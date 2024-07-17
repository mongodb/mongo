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

        self.assertEqual(bfs_report.correctness.hot["team-1"], {"BF-1"})
        self.assertEqual(len(bfs_report.correctness.cold.keys()), 0)
        self.assertEqual(len(bfs_report.correctness.none.keys()), 0)
        self.assertEqual(len(bfs_report.performance.hot.keys()), 0)
        self.assertEqual(len(bfs_report.performance.cold.keys()), 0)
        self.assertEqual(len(bfs_report.performance.none.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.hot.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.cold.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.none.keys()), 0)

    def test_add_hot_correctness_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["mongodb-mongo-master"],
            temperature=under_test.BfTemperature.HOT,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(bfs_report.correctness.hot["team-1"], {"BF-1"})
        self.assertEqual(len(bfs_report.correctness.cold.keys()), 0)
        self.assertEqual(len(bfs_report.correctness.none.keys()), 0)
        self.assertEqual(len(bfs_report.performance.hot.keys()), 0)
        self.assertEqual(len(bfs_report.performance.cold.keys()), 0)
        self.assertEqual(len(bfs_report.performance.none.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.hot.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.cold.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.none.keys()), 0)

    def test_add_cold_correctness_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["mongodb-mongo-master"],
            temperature=under_test.BfTemperature.COLD,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(len(bfs_report.correctness.hot.keys()), 0)
        self.assertEqual(bfs_report.correctness.cold["team-1"], {"BF-1"})
        self.assertEqual(len(bfs_report.correctness.none.keys()), 0)
        self.assertEqual(len(bfs_report.performance.hot.keys()), 0)
        self.assertEqual(len(bfs_report.performance.cold.keys()), 0)
        self.assertEqual(len(bfs_report.performance.none.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.hot.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.cold.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.none.keys()), 0)

    def test_add_hot_perf_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["sys-perf"],
            temperature=under_test.BfTemperature.HOT,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(len(bfs_report.correctness.hot.keys()), 0)
        self.assertEqual(len(bfs_report.correctness.cold.keys()), 0)
        self.assertEqual(len(bfs_report.correctness.none.keys()), 0)
        self.assertEqual(bfs_report.performance.hot["team-1"], {"BF-1"})
        self.assertEqual(len(bfs_report.performance.cold.keys()), 0)
        self.assertEqual(len(bfs_report.performance.none.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.hot.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.cold.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.none.keys()), 0)

    def test_add_cold_perf_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            evergreen_projects=["sys-perf"],
            temperature=under_test.BfTemperature.COLD,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1, evg_projects_info)

        self.assertEqual(len(bfs_report.correctness.hot.keys()), 0)
        self.assertEqual(len(bfs_report.correctness.cold.keys()), 0)
        self.assertEqual(len(bfs_report.correctness.none.keys()), 0)
        self.assertEqual(len(bfs_report.performance.hot.keys()), 0)
        self.assertEqual(bfs_report.performance.cold["team-1"], {"BF-1"})
        self.assertEqual(len(bfs_report.performance.none.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.hot.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.cold.keys()), 0)
        self.assertEqual(len(bfs_report.unknown.none.keys()), 0)
