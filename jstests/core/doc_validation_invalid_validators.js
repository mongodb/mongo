// Verify invalid validator statements won't work and that we
// can't create validated collections on restricted databases.
(function() {
    "use strict";

    var collName = "doc_validation_invalid_validators";
    var coll = db[collName];
    coll.drop();

    // Check a few invalid match statements for validator.
    assert.commandFailed(db.createCollection(collName, {validator: 7}));
    assert.commandFailed(db.createCollection(collName, {validator: "assert"}));

    // Check some disallowed match statements.
    assert.commandFailed(db.createCollection(collName, {validator: {$text: "bob"}}));
    assert.commandFailed(db.createCollection(collName, {validator: {$where: "this.a == this.b"}}));
    assert.commandFailed(db.createCollection(collName, {validator: {$near: {place: "holder"}}}));
    assert.commandFailed(db.createCollection(collName, {validator: {$geoNear: {place: "holder"}}}));
    assert.commandFailed(
        db.createCollection(collName, {validator: {$nearSphere: {place: "holder"}}}));

    // Verify we fail on admin, local and config databases.
    assert.commandFailed(
        db.getSiblingDB("admin").createCollection(collName, {validator: {a: {$exists: true}}}));
    if (!db.runCommand("isdbgrid").isdbgrid) {
        assert.commandFailed(
            db.getSiblingDB("local").createCollection(collName, {validator: {a: {$exists: true}}}));
    }
    assert.commandFailed(
        db.getSiblingDB("config").createCollection(collName, {validator: {a: {$exists: true}}}));

    // Create collection with document validator.
    assert.commandWorked(db.createCollection(collName, {validator: {a: {$exists: true}}}));

    // Verify some invalid match statements can't be passed to collMod.
    assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$text: "bob"}}));
    assert.commandFailed(
        db.runCommand({"collMod": collName, "validator": {$where: "this.a == this.b"}}));
    assert.commandFailed(
        db.runCommand({"collMod": collName, "validator": {$near: {place: "holder"}}}));
    assert.commandFailed(
        db.runCommand({"collMod": collName, "validator": {$geoNear: {place: "holder"}}}));
    assert.commandFailed(
        db.runCommand({"collMod": collName, "validator": {$nearSphere: {place: "holder"}}}));

    coll.drop();

    // Create collection without document validator.
    assert.commandWorked(db.createCollection(collName));

    // Verify we can't add an invalid validator to a collection without a validator.
    assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$text: "bob"}}));
    assert.commandFailed(
        db.runCommand({"collMod": collName, "validator": {$where: "this.a == this.b"}}));
    assert.commandWorked(db.runCommand({"collMod": collName, "validator": {a: {$exists: true}}}));
    coll.drop();
})();
