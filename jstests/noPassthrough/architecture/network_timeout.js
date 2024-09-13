/**
 * Tests various connection timeouts in and below the NetworkInterface layer return with
 * different error codes. The fail points aim to mimic time outs in different locations, while
 * cluster `find` will use a higher level API that will retry on retryable error codes.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Skip various checks that require talking to shard primaries (a primary is dropped as part
// of the test).
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

let st = new ShardingTest({shards: 1, mongos: 1, config: 1, rs: {nodes: 2}});
let testDB = "test";
let testColl = "testColl";
let testNS = testDB + "." + testColl;
let admin = st.s.getDB("admin");

let conn = new Mongo(st.s.host);

// Shard the collection to test sharding APIs such as AsyncRequestsSender during cluster find.
assert.commandWorked(admin.runCommand({enableSharding: testDB}));
assert.commandWorked(admin.runCommand({shardCollection: testNS, key: {_id: 1}}));

let coll = conn.getCollection(testNS);

// Make sure insert occurs on both nodes.
var wc = {writeConcern: {w: 2, wtimeout: 60000}};
assert.commandWorked(coll.insert({_id: 1}, wc));

assert.neq(null, coll.findOne({_id: 1}));

/** Testing that timeouts in certain locations return retriable errors. */

/**
 * Mimic timeout before handshake finishes. The error code is retryable, so the findOne should
 * succeed.
 */
// Ensure all connections dropped so unused connections aren't immediately reused.
configureFailPoint(st.s,
                   "connectionPoolDropConnectionsBeforeGetConnection",
                   {"instance": "NetworkInterfaceTL-TaskExecutor"},
                   "alwaysOn");
configureFailPoint(st.s,
                   "triggerConnectionSetupHandshakeTimeout",
                   {"instance": "NetworkInterfaceTL-TaskExecutor"},
                   {times: 1});
assert.neq(null, coll.findOne({_id: 1}));
configureFailPoint(st.s, "connectionPoolDropConnectionsBeforeGetConnection", {}, "off");

/**
 * Mimic timeout from timer in the NetworkInterface. This general timeout should not return a
 * retryable error.
 */
configureFailPoint(
    st.s, "triggerSendRequestNetworkTimeout", {"collectionNS": testColl}, {times: 1});
// Run a long query to make sure the network fail points fail the command.
assert.throwsWithCode(function() {
    coll.findOne({
        _id: 1,
        $where: function() {
            const start = new Date().getTime();
            while (new Date().getTime() - start < 100000)
                ;
            return true;
        }
    });
}, ErrorCodes.NetworkInterfaceExceededTimeLimit);

/** Testing host unreachable or network timeout results in query being retried on secondary. */

// Secondaries do not refresh their in-memory routing table until a request with a higher
// version is received, and refreshing requires communication with the primary to obtain the
// newest version. Read from the secondary once before taking down primary to ensure they
// have loaded the routing table into memory.
conn.setReadPref("secondary");
assert(coll.find({}).hasNext());

st.rs0.stop(st.rs0.getPrimary());
conn.setSecondaryOk();
conn.setReadPref("primaryPreferred");
assert.neq(null, coll.findOne({_id: 1}));

st.stop();
