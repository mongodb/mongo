// Tests that the internalValidateFeaturesAsMaster server parameter works properly even though
// it is deprecated. The preferred parameter is internalValidateFeaturesAsPrimary, which is
// tested in internal_validate_features_as_primary.js.

(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");

// internalValidateFeaturesAsMaster can be set via startup parameter.
let conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsMaster=1"});
assert.neq(null, conn, "mongod was unable to start up");
let res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
assert.commandWorked(res);
assert.eq(res.internalValidateFeaturesAsMaster, true);
MongoRunner.stopMongod(conn);

// internalValidateFeaturesAsMaster cannot be set with --replSet.
assert.throws(() => MongoRunner.runMongod(
                  {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=0"}),
              [],
              "mongod was unexpectedly able to start up");

assert.throws(() => MongoRunner.runMongod(
                  {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=1"}),
              [],
              "mongod was unexpectedly able to start up");

// internalValidateFeaturesAsMaster cannot be set via runtime parameter.
conn = MongoRunner.runMongod({});
assert.commandFailed(conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: true}));
assert.commandFailed(conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: false}));
MongoRunner.stopMongod(conn);
}());
