/*
 * Auth test for the truncateRange command wrapped in an applyOps command, running against
 * mongod.
 * @tags: [
 * featureFlagUseReplicatedTruncatesForDeletions,
 * requires_fcv_83
 * ]
 */
import {runTest} from "jstests/auth/lib/truncate_range_base.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
