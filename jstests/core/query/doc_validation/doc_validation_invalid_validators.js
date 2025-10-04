// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: eval.
//   not_allowed_with_signed_security_token,
//   assumes_no_implicit_collection_creation_after_drop,
//   requires_non_retryable_commands,
//   # $text is not supported on views.
//   incompatible_with_views,
// ]

// Verify invalid validator statements won't work and that we
// can't create validated collections on restricted databases.
let collName = "doc_validation_invalid_validators";
let coll = db[collName];
coll.drop();

// Check a few invalid match statements for validator.
assert.commandFailed(db.createCollection(collName, {validator: 7}));
assert.commandFailed(db.createCollection(collName, {validator: "assert"}));
assert.commandFailed(db.createCollection(collName, {validator: {$jsonSchema: {invalid: 1}}}));

// Check some disallowed match statements.
assert.commandFailed(db.createCollection(collName, {validator: {$text: "bob"}}));
assert.commandFailed(db.createCollection(collName, {validator: {$where: "this.a == this.b"}}));
assert.commandFailed(db.createCollection(collName, {validator: {$near: {place: "holder"}}}));
assert.commandFailed(db.createCollection(collName, {validator: {$geoNear: {place: "holder"}}}));
assert.commandFailed(db.createCollection(collName, {validator: {$nearSphere: {place: "holder"}}}));
assert.commandFailed(db.createCollection(collName, {validator: {$expr: {$eq: ["$a", "$$unbound"]}}}));
assert.commandFailed(db.createCollection(collName, {validator: {$expr: {$coerceToBool: {$eq: ["$_id", 15]}}}}));

// Verify we fail on admin, local and config databases.
assert.commandFailed(db.getSiblingDB("admin").createCollection(collName, {validator: {a: {$exists: true}}}));
if (!db.runCommand("isdbgrid").isdbgrid) {
    assert.commandFailed(db.getSiblingDB("local").createCollection(collName, {validator: {a: {$exists: true}}}));
}
assert.commandFailed(db.getSiblingDB("config").createCollection(collName, {validator: {a: {$exists: true}}}));

// Create collection with document validator.
assert.commandWorked(db.createCollection(collName, {validator: {a: {$exists: true}}}));

// Verify some invalid match statements can't be passed to collMod.
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$text: {$search: "bob"}}}));
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$where: "this.a == this.b"}}));
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "validator": {
            $expr: {$function: {body: "function(age) { return age >= 21; }", args: ["$age"], lang: "js"}},
        },
    }),
    4660800,
);
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "validator": {$expr: {$_internalJsEmit: {eval: "function() {}", this: {}}}},
    }),
    4660801,
);
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$near: {place: "holder"}}}));
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$geoNear: {place: "holder"}}}));
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$nearSphere: {place: "holder"}}}));
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$expr: {$eq: ["$a", "$$unbound"]}}}));
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$jsonSchema: {invalid: 7}}}));

coll.drop();

// Create collection without document validator.
assert.commandWorked(db.createCollection(collName));

// Verify we can't add an invalid validator to a collection without a validator.
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$text: "bob"}}));
assert.commandFailed(db.runCommand({"collMod": collName, "validator": {$where: "this.a == this.b"}}));
assert.commandWorked(db.runCommand({"collMod": collName, "validator": {a: {$exists: true}}}));
coll.drop();
