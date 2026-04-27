/*
 * Auth test for the truncateRange command wrapped in an applyOps command, running in a replica set.
 * @tags: [
 * featureFlagUseReplicatedTruncatesForDeletions,
 * requires_replication,
 * requires_fcv_83
 * ]
 */
import {runTest} from "jstests/auth/lib/truncate_range_base.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({name: jsTestName(), nodes: 2, keyFile: "jstests/libs/key1"});
replTest.startSet();
replTest.initiate();
runTest(replTest.getPrimary());
replTest.stopSet();
