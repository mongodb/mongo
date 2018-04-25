// Verify custom roles still exist after a FeatureCompatability upgrade from 3.4 to 3.6

(function() {
    'use strict';
    print("START auth-feature-compat-36-roles-collection.js");
    var db = MongoRunner.runMongod({}).getDB("test");

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandWorked(db.runCommand(
        {createRole: "role1", roles: [{role: "readWrite", db: "test"}], privileges: []}));
    assert(db.runCommand({rolesInfo: "role1"}).roles[0].role === "role1");
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Verify that the role still exists after FC upgrade to 3.6
    assert(db.runCommand({rolesInfo: "role1"}).roles[0].role === "role1");

    MongoRunner.stopMongod(db.getMongo());
    print("SUCCESS auth-feature-compat-36-roles-collection.js");
})();
