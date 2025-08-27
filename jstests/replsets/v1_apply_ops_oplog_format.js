/**
 * These tests verify that we don't create v1 oplog entries when applying v1 updates supplied to
 * applyOps.
 *
 * @tags: [
 *    # Applying v1 oplog entries with applyOps is only supported in 8.0 onwards.
 *    requires_fcv_80,
 * ]
 */

const rst = new ReplSetTest({nodes: 1, oplogSize: 2});
const nodes = rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const db = primary.getDB(jsTestName() + "_db");
const coll = db[jsTestName() + "_coll"];

function getLastOplogEntry() {
    return primary.getDB("local").oplog.rs.find().limit(1).sort({$natural: -1}).next();
}

const assertLastOplog = function(o, o2, msg) {
    const last = getLastOplogEntry();

    assert.eq(last.ns, coll.getFullName(), "ns bad : " + msg);
    assert.docEq(last.o, o, "o bad : " + msg);
    if (o2)
        assert.docEq(last.o2, o2, "o2 bad : " + msg);
    return last.ts;
};

// Initialize the document we want to modify.
assert.commandWorked(coll.insert({_id: 1}));
assertLastOplog({_id: 1}, null, "insert -- setup");

{
    const msg = "Explicitly versioned v1 entry: $set";
    const res = assert.commandWorked(primary.adminCommand({
        "applyOps": [{
            "op": "u",
            "ns": coll.getFullName(),
            "o2": {"_id": 1},
            "o": {"$v": 1, "$set": {"a": 1}}
        }]
    }));
    assert.eq(res.results, [true], msg);
    assertLastOplog({"$v": 2, "diff": {"i": {"a": 1}}}, {"_id": 1}, msg);
}

{
    const msg = "Explicitly versioned v1 entry: $unset";
    const res = assert.commandWorked(primary.adminCommand({
        "applyOps": [{
            "op": "u",
            "ns": coll.getFullName(),
            "o2": {"_id": 1},
            "o": {"$v": 1, "$unset": {"a": ""}}
        }]
    }));
    assert.eq(res.results, [true], msg);
    assertLastOplog({"$v": 2, "diff": {"d": {"a": false}}}, {"_id": 1}, msg);
}

{
    const msg = "Implicitly versioned v1 entry: $set";
    const res = assert.commandWorked(primary.adminCommand({
        "applyOps":
            [{"op": "u", "ns": coll.getFullName(), "o2": {"_id": 1}, "o": {"$set": {"b": 1}}}]
    }));
    assert.eq(res.results, [true], msg);
    assertLastOplog({"$v": 2, "diff": {"i": {"b": 1}}}, {"_id": 1}, msg);
}

{
    const msg = "Implicitly versioned v1 entry: $unset";
    const res = assert.commandWorked(primary.adminCommand({
        "applyOps":
            [{"op": "u", "ns": coll.getFullName(), "o2": {"_id": 1}, "o": {"$unset": {"b": ""}}}]
    }));
    assert.eq(res.results, [true], msg);
    assertLastOplog({"$v": 2, "diff": {"d": {"b": false}}}, {"_id": 1}, msg);
}

rst.stopSet();
