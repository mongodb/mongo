/**
 * Tests that applyOps correctly respects the 'oplogApplicationMode' and 'alwaysUpsert' flags.
 * 'alwaysUpsert' defaults to false and 'oplogApplicationMode' defaults to 'ApplyOps'. We test
 * that these default values do not lead to command failure.
 */

let standalone = MongoRunner.runMongod();
var db = standalone.getDB("test");

let coll = db.getCollection("apply_ops_mode1");

// ------------ Testing normal updates ---------------

let id = ObjectId();
for (let updateOp of [
    // An update with a modifier.
    {op: "u", ns: coll.getFullName(), o: {$v: 2, diff: {u: {x: 1}}}, o2: {_id: id}},
    // A full-document replace.
    {op: "u", ns: coll.getFullName(), o: {_id: id, x: 1}, o2: {_id: id}},
]) {
    coll.drop();
    assert.writeOK(coll.insert({_id: 1}));

    jsTestLog(`Test applyOps with the following op:\n${tojson(updateOp)}`);
    assert.commandFailed(db.adminCommand({applyOps: [updateOp]}));
    assert.eq(coll.count({x: 1}), 0);

    // Test that 'InitialSync' does not override (default) 'alwaysUpsert: false'.
    assert.commandFailed(db.adminCommand({applyOps: [updateOp], oplogApplicationMode: "InitialSync"}));
    assert.eq(coll.count({x: 1}), 0);

    // Test parsing failure.
    assert.commandFailedWithCode(
        db.adminCommand({applyOps: [updateOp], oplogApplicationMode: "BadMode"}),
        ErrorCodes.FailedToParse,
    );
    assert.commandFailedWithCode(
        db.adminCommand({applyOps: [updateOp], oplogApplicationMode: 5}),
        ErrorCodes.TypeMismatch,
    );
}

// ------------ Testing fCV updates ---------------

let adminDB = db.getSiblingDB("admin");
const systemVersionColl = adminDB.getCollection("system.version");

let updateOp = {
    op: "u",
    ns: systemVersionColl.getFullName(),
    o: {_id: "featureCompatibilityVersion", version: lastLTSFCV},
    o2: {_id: "featureCompatibilityVersion"},
};
assert.commandFailed(db.adminCommand({applyOps: [updateOp], oplogApplicationMode: "InitialSync"}));

assert.commandWorked(db.adminCommand({applyOps: [updateOp], oplogApplicationMode: "ApplyOps"}));

// Test default succeeds.
updateOp.o.targetVersion = latestFCV;
assert.commandWorked(
    db.adminCommand({
        applyOps: [updateOp],
    }),
);

// ------------ Testing commands on the fCV collection ---------------

let collModOp = {
    op: "c",
    ns: systemVersionColl.getDB() + ".$cmd",
    o: {collMod: systemVersionColl.getName(), validationLevel: "off"},
};
assert.commandFailed(db.adminCommand({applyOps: [collModOp], oplogApplicationMode: "InitialSync"}));

assert.commandWorked(db.adminCommand({applyOps: [collModOp], oplogApplicationMode: "ApplyOps"}));

// Test default succeeds.
assert.commandWorked(
    db.adminCommand({
        applyOps: [collModOp],
    }),
);

MongoRunner.stopMongod(standalone);
