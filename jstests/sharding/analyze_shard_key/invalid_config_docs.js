/**
 * Tests that writing an invalid config.queryAnalyzers or config.mongos document only causes the
 * write to fail (i.e. doesn't cause the server to crash).
 *
 * @tags: [
 *    requires_fcv_70,
 *    # TODO (SERVER-88122): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runAnalyzerDocTest(conn) {
    const configColl = conn.getCollection("config.queryAnalyzers");
    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandFailedWithCode(configColl.insert({_id: ns, unknownField: 0}),
                                 ErrorCodes.IDLFailedToParse /* IDL required field error */);

    assert.commandWorked(conn.getDB(dbName).createCollection(collName));
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1}));
    assert.commandFailedWithCode(configColl.update({}, {unknownField: 0}),
                                 ErrorCodes.IDLFailedToParse /* IDL required field error */);
}

function runMongosDocTest(conn) {
    const configColl = conn.getCollection("config.mongos");
    assert.commandFailedWithCode(configColl.insert({_id: "mongos0"}), ErrorCodes.NoSuchKey);

    jsTest.log("Wait for the mongos to report its uptime, i.e. for its config.mongos document " +
               "to exist. Otherwise, the update below would be a no-op and not fail");
    assert.soon(() => {
        return configColl.find().itcount() == 1;
    });

    assert.commandFailedWithCode(configColl.update({}, {unknownField: 0}), ErrorCodes.NoSuchKey);
}

{
    const st = new ShardingTest({shards: 1});
    runAnalyzerDocTest(st.s);
    runMongosDocTest(st.s);
    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    runAnalyzerDocTest(primary);
    rst.stopSet();
}
