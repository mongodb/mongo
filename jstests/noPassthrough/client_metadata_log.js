/**
 * Test that verifies client metadata is logged into log file on new connections.
 * @tags: [
 *   requires_sharding,
 * ]
 */

(function() {
'use strict';

let checkLog = function(conn) {
    let coll = conn.getCollection("test.foo");
    assert.commandWorked(coll.insert({_id: 1}));

    print(`Checking ${conn.fullOptions.logFile} for client metadata message`);
    let log = cat(conn.fullOptions.logFile);

    const predicate =
        /"id":51800,.*"msg":"client metadata","attr":.*"doc":{"application":{"name":".*"},"driver":{"name":".*","version":".*"},"os":{"type":".*","name":".*","architecture":".*","version":".*"}}/;

    assert(predicate.test(log),
           "'client metadata' log line missing in log file!\n" +
               "Log file contents: " + conn.fullOptions.logFile +
               "\n************************************************************\n" + log +
               "\n************************************************************");
};

// Test MongoD
let testMongoD = function() {
    let conn = MongoRunner.runMongod({useLogFiles: true});
    assert.neq(null, conn, 'mongod was unable to start up');

    checkLog(conn);

    MongoRunner.stopMongod(conn);
};

// Test MongoS
let testMongoS = function() {
    let options = {
        mongosOptions: {useLogFiles: true},
    };

    let st = new ShardingTest({shards: 1, mongos: 1, other: options});

    checkLog(st.s0);

    // Validate db.currentOp() contains mongos information
    let curOp = st.s0.adminCommand({currentOp: 1});
    print(tojson(curOp));

    var inprogSample = null;
    for (let inprog of curOp.inprog) {
        if (inprog.hasOwnProperty("clientMetadata") &&
            inprog.clientMetadata.hasOwnProperty("mongos")) {
            inprogSample = inprog;
            break;
        }
    }

    assert.neq(inprogSample.clientMetadata.mongos.host, "unknown");
    assert.neq(inprogSample.clientMetadata.mongos.client, "unknown");
    assert.neq(inprogSample.clientMetadata.mongos.version, "unknown");

    st.stop();
};

testMongoD();
testMongoS();
})();
