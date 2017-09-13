// Tests the internalValidateFeaturesAsMaster server parameter.

(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    // internalValidateFeaturesAsMaster can be set via startup parameter.
    let conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.neq(null, conn, "mongod was unable to start up");
    let res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, true);
    MongoRunner.stopMongod(conn);

    // internalValidateFeaturesAsMaster cannot be set with --replSet, --master, or --slave.
    conn = MongoRunner.runMongod(
        {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod(
        {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({master: "", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({master: "", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({slave: "", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({slave: "", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    // internalValidateFeaturesAsMaster cannot be set via runtime parameter.
    conn = MongoRunner.runMongod({});
    assert.commandFailed(
        conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: true}));
    assert.commandFailed(
        conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: false}));
    MongoRunner.stopMongod(conn);
}());
