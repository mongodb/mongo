/**
 * Tests restarting the server and then shutting down uncleanly, both times recovering from a
 * timestamp before the commit timestamp of an index build.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";
import {MissingIndexIdent} from "jstests/noPassthrough/libs/missing_index_ident.js";

const {replTest, ts, ident} = MissingIndexIdent.run();

replTest.start(0, undefined, true /* restart */);
const coll = replTest.getPrimary().getDB('test')[jsTestName()];

MissingIndexIdent.checkRecoveryLogs(replTest.getPrimary(), coll, ts, ident);
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

replTest.stopSet();
