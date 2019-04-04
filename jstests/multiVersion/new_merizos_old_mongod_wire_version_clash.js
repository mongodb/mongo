/**
 * Verify that a current merizos, when connected to an old merizod (one that
 * implements a different wire-protocol version) reports the resulting failures
 * properly.
 *
 * Note that the precise errors and failure modes caught here are not documented,
 * and are not depended upon by deployed systems.  If improved error handling
 * results in this test failing, this test may be updated to reflect the actual
 * error reported.  In particular, a change that causes a failure to report
 * ErrorCodes.IncompatibleServerVersion here would generally be an improvement.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {

    'use strict';

    /*  Start a ShardingTest with a 'last-stable' merizos so that a 'last-stable'
     *  shard can be added.  (A 'last-stable' shard cannot be added from a
     *  current merizos because the wire protocol must be presumed different.)
     */
    var st = new ShardingTest({
        shards: 1,
        other: {
            merizosOptions: {binVersion: 'last-stable'},
            shardOptions: {binVersion: 'last-stable'},
            shardAsReplicaSet: false
        }
    });

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));

    // Start a current-version merizos.
    var newMerizos = MerizoRunner.runMerizos({configdb: st._configDB});

    // Write commands report failure by returning writeError:

    assert.writeErrorWithCode(newMerizos.getDB('test').foo.insert({x: 1}),
                              ErrorCodes.IncompatibleServerVersion);

    assert.writeErrorWithCode(newMerizos.getDB('test').foo.update({x: 1}, {x: 1, y: 2}),
                              ErrorCodes.IncompatibleServerVersion);

    assert.writeErrorWithCode(newMerizos.getDB('test').foo.remove({x: 1}),
                              ErrorCodes.IncompatibleServerVersion);

    // Query commands, on failure, throw instead:

    let res;
    res = newMerizos.getDB('test').runCommand({find: 'foo'});
    assert.eq(res.code, ErrorCodes.IncompatibleServerVersion);

    res = newMerizos.getDB('test').runCommand({find: 'foo', filter: {x: 1}});
    assert.eq(res.code, ErrorCodes.IncompatibleServerVersion);

    res = newMerizos.getDB('test').runCommand({aggregate: 'foo', pipeline: [], cursor: {}});
    assert.eq(res.code, ErrorCodes.IncompatibleServerVersion);

    MerizoRunner.stopMerizos(newMerizos);
    st.stop();

})();
