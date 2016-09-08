/**
 * Verify that a current mongos, when connected to an old mongod (one that
 * implements a different wire-protocol version) reports the resulting failures
 * properly.
 *
 * Note that the precise errors and failure modes caught here are not documented,
 * and are not depended upon by deployed systems.  If improved error handling
 * results in this test failing, this test may be updated to reflect the actual
 * error reported.  In particular, a change that causes a failure to report
 * ErrorCodes.IncompatibleServerVersion here would generally be an improvement.
 */

(function() {

    "use strict";

    load('jstests/libs/override_methods/multiversion_override_balancer_control.js');

    /*  Start a ShardingTest with a 'last-stable' mongos so that a 'last-stable'
     *  shard can be added.  (A 'last-stable' shard cannot be added from a
     *  current mongos because the wire protocol must be presumed different.)
     */
    var st = new ShardingTest({
        shards: 1,
        other: {
            mongosOptions: {binVersion: "last-stable"},
            shardOptions: {binVersion: "last-stable"},
        }
    });

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));

    // Start a current-version mongos.
    var newMongos = MongoRunner.runMongos({configdb: st._configDB});

    // Write commands report failure by returning writeError:

    assert.writeErrorWithCode(newMongos.getDB('test').foo.insert({x: 1}),
                              ErrorCodes.RemoteResultsUnavailable);

    assert.writeErrorWithCode(newMongos.getDB('test').foo.update({x: 1}, {x: 1, y: 2}),
                              ErrorCodes.RemoteResultsUnavailable);

    assert.writeErrorWithCode(newMongos.getDB('test').foo.remove({x: 1}),
                              ErrorCodes.RemoteResultsUnavailable);

    // Query commands, on failure, throw instead:

    var thrownException = assert.throws(function() {
        newMongos.getDB('test').foo.find({}).next();
    });
    assert.eq(ErrorCodes.IncompatibleServerVersion, thrownException.code);

    var socketExceptionErrorCode = 11002;

    thrownException = assert.throws(function() {
        newMongos.getDB('test').foo.find({x: 1}).count();
    });
    assert.eq(socketExceptionErrorCode, thrownException.code);

    thrownException = assert.throws(function() {
        newMongos.getDB('test').foo.aggregate([]);
    });
    assert.eq(socketExceptionErrorCode, thrownException.code);

    MongoRunner.stopMongos(newMongos.port);
    st.stop();

})();
