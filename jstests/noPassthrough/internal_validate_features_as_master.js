// Tests the internalValidateFeaturesAsMaster server parameter.

(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    // internalValidateFeaturesAsMaster can be set via startup parameter.
    let conn = MerizoRunner.runMerizod({setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.neq(null, conn, "merizod was unable to start up");
    let res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, true);
    MerizoRunner.stopMerizod(conn);

    // internalValidateFeaturesAsMaster cannot be set with --replSet.
    conn = MerizoRunner.runMerizod(
        {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "merizod was unexpectedly able to start up");

    conn = MerizoRunner.runMerizod(
        {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "merizod was unexpectedly able to start up");

    // internalValidateFeaturesAsMaster cannot be set via runtime parameter.
    conn = MerizoRunner.runMerizod({});
    assert.commandFailed(
        conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: true}));
    assert.commandFailed(
        conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: false}));
    MerizoRunner.stopMerizod(conn);
}());
