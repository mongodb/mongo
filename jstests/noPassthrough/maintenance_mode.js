/*
 * Test that mongod accepts the --maintenanceMode parameter.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

// --maintenanceMode=replicaSet should succeed
let node = MongoRunner.runMongod({
    maintenanceMode: "replicaSet",
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    },
});
MongoRunner.stopMongod(node);

// --maintenanceMode=standalone should succeed
node = MongoRunner.runMongod({
    maintenanceMode: "standalone",
    setParameter: {
        featureFlagAllMongodsAreSharded: true,
    },
});
MongoRunner.stopMongod(node);

// --maintenanceMode should fail
assert.throws(() => {
    MongoRunner.runMongod({
        maintenanceMode: "",
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });
});

// --maintenanceMode=nonValidString should fail
assert.throws(() => {
    MongoRunner.runMongod({
        maintenanceMode: "nonValidString",
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });
});
})();
