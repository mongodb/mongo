/**
 * Test bulk inserts running alonside the auto-balancer. Ensures that they do not conflict with each
 * other.
 *
 * This test is labeled resource intensive because its total io_write is ~26MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [resource_intensive]
 */
(function() {
'use strict';
load("jstests/libs/fail_point_util.js");

var st = new ShardingTest({shards: 4, chunkSize: 1});

// Double the balancer interval to produce fewer migrations per unit time so that the test does not
// run out of stale shard version retries.
configureFailPointForRS(
    st.configRS.nodes, 'overrideBalanceRoundInterval', {intervalMs: 2000}, 'alwaysOn');

assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
st.ensurePrimaryShard('TestDB', st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {Counter: 1}}));

var db = st.s0.getDB('TestDB');
var coll = db.TestColl;

// Insert lots of bulk documents
var numDocs = 250000;

var bulkSize = 4000;
var docSize = 128; /* bytes */
print("\n\n\nBulk size is " + bulkSize);

var data = "x";
while (Object.bsonsize({x: data}) < docSize) {
    data += data;
}

print("\n\n\nDocument size is " + Object.bsonsize({x: data}));

var docsInserted = 0;
var balancerOn = false;

/**
 * Ensures that the just inserted documents can be found.
 */
function checkDocuments() {
    var docsFound = coll.find({}, {_id: 0, Counter: 1}).toArray();
    var count = coll.find().count();

    if (docsFound.length != docsInserted) {
        print("Inserted " + docsInserted + " count : " + count +
              " doc count : " + docsFound.length);

        var allFoundDocsSorted = docsFound.sort(function(a, b) {
            return a.Counter - b.Counter;
        });

        var missingValueInfo;

        for (var i = 0; i < docsInserted; i++) {
            if (i != allFoundDocsSorted[i].Counter) {
                missingValueInfo = {expected: i, actual: allFoundDocsSorted[i].Counter};
                break;
            }
        }

        st.printShardingStatus();

        assert(
            false,
            'Inserted number of documents does not match the actual: ' + tojson(missingValueInfo));
    }
}

while (docsInserted < numDocs) {
    var currBulkSize = (numDocs - docsInserted > bulkSize) ? bulkSize : (numDocs - docsInserted);

    var bulk = [];
    for (var i = 0; i < currBulkSize; i++) {
        bulk.push({Counter: docsInserted, hi: "there", i: i, x: data});
        docsInserted++;
    }

    assert.commandWorked(coll.insert(bulk));

    if (docsInserted % 10000 == 0) {
        print("Inserted " + docsInserted + " documents.");
        st.printShardingStatus();
    }

    if (docsInserted > numDocs / 3 && !balancerOn) {
        // Do one check before we turn balancer on
        checkDocuments();
        print('Turning on balancer after ' + docsInserted + ' documents inserted.');
        st.startBalancer();
        balancerOn = true;
    }
}

checkDocuments();

st.stop();
})();
