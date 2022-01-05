// SERVER-35132 Test that we still honor maxTimeMs during replica set targeting.
// @tags: [
//   requires_replication,
// ]
(function() {
'use strict';
var st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 2}});
var kDbName = 'test';
var ns = 'test.foo';
var mongos = st.s0;
var testColl = mongos.getCollection(ns);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

// Since this test is timing sensitive, retry on failures since they could be transient.
// If broken, this would *always* fail so if it ever passes this build is fine (or time went
// backwards).
const retryOnFailureUpToFiveTimes = function(name, f) {
    jsTestLog(`Starting test ${name}`);

    for (let trial = 1; trial <= 5; trial++) {
        try {
            f();
            break;
        } catch (e) {
            if (trial < 5) {
                jsTestLog(`Ignoring error during trial ${trial} of test ${name}`);
                continue;
            }

            jsTestLog(`Failed 5 times in test ${
                name}. There is probably a bug here. Latest assertion: ${tojson(e)}`);
            throw e;
        }
    }
};

const runTest = function() {
    // Sanity Check
    assert.eq(testColl.find({_id: 1}).next(), {_id: 1});

    // MaxTimeMS with satisfiable readPref
    assert.eq(testColl.find({_id: 1}).readPref("secondary").maxTimeMS(1000).next(), {_id: 1});

    let ex = null;

    // MaxTimeMS with unsatisfiable readPref
    const time = Date.timeFunc(() => {
        ex = assert.throws(() => {
            testColl.find({_id: 1})
                .readPref("secondary", [{tag: "noSuchTag"}])
                .maxTimeMS(1000)
                .next();
        });
    });

    assert.gte(time, 1000);      // Make sure we at least waited 1 second.
    assert.lt(time, 15 * 1000);  // We used to wait 20 seconds before timing out.

    assert.eq(ex.code, ErrorCodes.MaxTimeMSExpired);
};

testColl.insert({_id: 1}, {writeConcern: {w: 2}});
retryOnFailureUpToFiveTimes("totally unsharded", runTest);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
retryOnFailureUpToFiveTimes("sharded db", runTest);

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
retryOnFailureUpToFiveTimes("sharded collection", runTest);

st.stop();
})();
