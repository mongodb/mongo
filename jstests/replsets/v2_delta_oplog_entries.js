/**
 * Tests use of $v: 2 delta style oplog entries for pipeline based updates. This test only checks
 * steady-state replication cases. It does not attempt to target cases where delta entries are
 * re-applied as part of initial sync or rollback.
 *
 * This test relies on the DBHash checker to run at the end to ensure that the primaries and
 * secondaries have the same data. For that reason it's important that this test not drop
 * intermediate collections.
 *
 * @tags: [requires_fcv_46]
 */
(function() {
const rst = new ReplSetTest({
    name: "v2_delta_oplog_entries",
    nodes: 2,
    nodeOptions: {setParameter: {internalQueryEnableLoggingV2OplogEntries: true}}
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryColl = primary.getDB("test").coll;
const secondary = rst.getSecondary();
const secondaryColl = secondary.getDB("test").coll;

// Used for padding documents, in order to make full replacements expensive.
function makeGiantStr() {
    let s = "";
    for (let i = 0; i < 1024; i++) {
        s += "_";
    }
    return s;
}

const kGiantStr = makeGiantStr();
const kMediumLengthStr = "zzzzzzzzzzzzzzzzzzzzzzzzzz";

let idGenGlob = 0;
function generateId() {
    return idGenGlob++;
}

const kExpectDeltaEntry = "expectDelta";
const kExpectReplacementEntry = "expectReplacement";
// Indicates that the update ran was a noop and we should not expect to see a 'u' oplog
// entry.
const kExpectNoUpdateEntry = "expectNoEntry";

/**
 * Given a connection to a node, check that the most recent oplog entry for document with
 * 'expectedId' matches the type 'expectedOplogEntryType'.
 */
function checkOplogEntry(node, expectedOplogEntryType, expectedId) {
    const oplog = node.getDB("local").getCollection("oplog.rs");

    const res = oplog
                    .find({
                        $and: [
                            {ns: primaryColl.getFullName()},
                            {$or: [{"o._id": expectedId}, {"o2._id": expectedId}]}
                        ]
                    })
                    .limit(1)
                    .hint({$natural: -1})  // Reverse scan, so we get the most recent entry.
                    .toArray();
    assert.eq(res.length, 1);

    const oplogEntry = res[0];

    if (expectedOplogEntryType === kExpectDeltaEntry) {
        assert.eq(oplogEntry.op, "u");
        assert.eq(oplogEntry.o.$v, 2, oplogEntry);
        assert.eq(typeof (oplogEntry.o.diff), "object", oplogEntry);

        // Check that the oplog entry's _id field is for the document we updated.
        assert.eq(oplogEntry.o2._id, expectedId);

        // Do some cursory/weak checks about the format of the 'o' field.
        assert.eq(Object.keys(oplogEntry.o), ["$v", "diff"]);
        for (let key of Object.keys(oplogEntry.o.diff)) {
            assert.contains(key[0], ["i", "u", "s", "d"]);
        }
    } else if (expectedOplogEntryType === kExpectReplacementEntry) {
        assert.eq(oplogEntry.op, "u");
        assert.eq(oplogEntry.o.hasOwnProperty("$v"), false, oplogEntry);
    } else if (expectedOplogEntryType == kExpectNoUpdateEntry) {
        assert.eq(oplogEntry.op, "i");
        assert.eq(oplogEntry.o._id, expectedId);
    }
}

// Last parameter is whether we expect the oplog entry to only record an update rather than
// replacement.
function testUpdateReplicates({preImage, pipeline, postImage, expectedOplogEntry}) {
    const idKey = preImage._id;
    assert.commandWorked(primaryColl.insert(preImage));
    assert.commandWorked(primaryColl.update({_id: idKey}, pipeline));

    rst.awaitReplication();
    const secondaryDoc = secondaryColl.findOne({_id: idKey});
    assert.eq(postImage, secondaryDoc);

    checkOplogEntry(primary, expectedOplogEntry, preImage._id);
}

const oplog = primary.getDB("local").getCollection("oplog.rs");
let id;

// Removing fields.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: 3, y: 3, giantStr: kGiantStr},
    pipeline: [{$unset: ["x", "y"]}],
    postImage: {_id: id, giantStr: kGiantStr},
    expectedOplogEntry: kExpectDeltaEntry
});

// Adding a field and updating an existing one.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "notSoLargeString", y: 0},
    pipeline: [{$set: {a: "foo", y: 999}}],
    postImage: {_id: id, x: "notSoLargeString", y: 999, a: "foo"},
    expectedOplogEntry: kExpectDeltaEntry
});

// Updating a subfield to a string.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "notSoLargeString", subObj: {a: 1, b: 2}},
    pipeline: [{$set: {"subObj.a": "foo", y: 1}}],
    postImage: {_id: id, x: "notSoLargeString", subObj: {a: "foo", b: 2}, y: 1},
    expectedOplogEntry: kExpectDeltaEntry
});

// Updating a subfield to have the same value but different type. This is designed to check that the
// server uses strict binary comparison to determine whether a field needs to be updated, rather
// than a weak BSON type insensitive comparison.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "notSoLargeString", subObj: {a: NumberLong(1), b: 2}},
    pipeline: [{$set: {"subObj.a": 1, y: 1}}],
    postImage: {_id: id, x: "notSoLargeString", subObj: {a: 1, b: 2}, y: 1},
    expectedOplogEntry: kExpectDeltaEntry
});

// Update a subfield to an object.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "notSoLargeString", subObj: {a: NumberLong(1), b: 2}},
    pipeline: [{$set: {"subObj.a": {$const: {newObj: {subField: 1}}}, y: 1}}],
    postImage: {_id: id, x: "notSoLargeString", subObj: {a: {newObj: {subField: 1}}, b: 2}, y: 1},
    expectedOplogEntry: kExpectDeltaEntry
});

// Adding a field to a sub object.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, subObj: {a: 1, b: 2}},
    pipeline: [{$set: {"subObj.c": "foo"}}],
    postImage: {_id: id, subObj: {a: 1, b: 2, c: "foo"}},
    expectedOplogEntry: kExpectDeltaEntry
});

// Adding a field to a sub object while removing a top level field.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, subObj: {a: 1, b: 2}, toRemove: "foo", giantStr: kGiantStr},
    pipeline: [{$project: {subObj: 1, giantStr: 1}}, {$set: {"subObj.c": "foo"}}],
    postImage: {_id: id, subObj: {a: 1, b: 2, c: "foo"}, giantStr: kGiantStr},
    expectedOplogEntry: kExpectDeltaEntry
});

// Dropping a field via inclusion projection.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, toRemove: "foo", subObj: {a: 1, b: 2}},
    pipeline: [{$project: {subObj: 1}}],
    postImage: {_id: id, subObj: {a: 1, b: 2}},
    expectedOplogEntry: kExpectDeltaEntry
});

// Inclusion projection dropping a subfield (subObj.toRemove).
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "foo", subObj: {a: 1, toRemove: 2}, giantStr: kGiantStr},
    pipeline: [{$project: {subObj: {a: 1}, giantStr: 1}}],
    postImage: {_id: id, subObj: {a: 1}, giantStr: kGiantStr},
    expectedOplogEntry: kExpectDeltaEntry
});

// $replaceRoot with identical document. We should expect no update oplog entry in this case.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "foo", subObj: {a: 1, b: 2}},
    pipeline: [{$replaceRoot: {newRoot: {_id: id, x: "foo", subObj: {a: 1, b: 2}}}}],
    postImage: {_id: id, x: "foo", subObj: {a: 1, b: 2}},
    expectedOplogEntry: kExpectNoUpdateEntry
});

// $replaceRoot with a similar document. In this case the diff should be small enough to use
// delta oplog entries instead of doing a full replacement.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "foo", subObj: {a: 1, b: 2}, giantStr: kGiantStr},
    pipeline: [{$replaceRoot: {newRoot: {x: "bar", subObj: {a: 1, b: 2}, giantStr: kGiantStr}}}],
    postImage: {_id: id, x: "bar", subObj: {a: 1, b: 2}, giantStr: kGiantStr},
    expectedOplogEntry: kExpectDeltaEntry
});

// Replace root with a very different document. In this case we should fall back to a replacement
// style update.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "foo", subObj: {a: 1, b: 2}},
    pipeline: [{$replaceRoot: {newRoot: {_id: id, newField: kMediumLengthStr}}}],
    postImage: {_id: id, newField: kMediumLengthStr},
    expectedOplogEntry: kExpectReplacementEntry
});

// Combine updates to existing fields and insertions of new fields.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: 1, b: {c: 2, d: {e: 3, f: 6}}, z: 3},
    pipeline: [
        {$unset: ["b.d.f"]},
        {$set: {"b.a": 5, "b.b": 3, "b.c": 2, "b.d.d": 2, "b.d.e": 10, z: 7}}
    ],
    postImage: {_id: id, padding: kGiantStr, a: 1, b: {c: 2, d: {e: 10, d: 2}, a: 5, b: 3}, z: 7},
    expectedOplogEntry: kExpectDeltaEntry
});

// Setting a sub object inside an array.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: kGiantStr, arrField: [{x: 1}, {x: 2}]},
    pipeline: [{$set: {"arrField.x": 5}}],
    postImage: {_id: id, x: kGiantStr, arrField: [{x: 5}, {x: 5}]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Reordering fields with replaceRoot.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, x: "foo", y: "bar", z: "baz"},
    pipeline: [{$replaceRoot: {newRoot: {_id: id, z: "baz", y: "bar", x: "foo"}}}],
    postImage: {_id: id, z: "baz", y: "bar", x: "foo"},
    expectedOplogEntry: kExpectDeltaEntry
});

// Reordering two small fields in a very large document.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, x: "foo", y: "bar"},
    pipeline: [{$replaceRoot: {newRoot: {_id: id, padding: kGiantStr, y: "bar", x: "foo"}}}],
    postImage: {_id: id, padding: kGiantStr, y: "bar", x: "foo"},
    expectedOplogEntry: kExpectDeltaEntry
});

// Similar case of reordering fields.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, p: kGiantStr, a: 1, b: 1, c: 1, d: 1},
    pipeline: [{$replaceRoot: {newRoot: {_id: id, p: kGiantStr, a: 1, c: 1, b: 1, d: 1}}}],
    postImage: {_id: id, p: kGiantStr, a: 1, c: 1, b: 1, d: 1},
    expectedOplogEntry: kExpectDeltaEntry
});

// Modify an element in the middle of an array.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, 4, 5]},
    pipeline: [{$set: {a: [1, 2, 999, 4, 5]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, 2, 999, 4, 5]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Modify an object inside an array.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, {b: 1}, 5]},
    pipeline: [{$set: {a: [1, 2, 3, {b: 2}, 5]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, {b: 2}, 5]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Object inside an array inside an object inside an array.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, {b: [{c: 1}]}, 5]},
    pipeline: [{$set: {a: [1, 2, 3, {b: [{c: 999}]}, 5]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, {b: [{c: 999}]}, 5]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Case where we append to an array.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, 2, 3]},
    pipeline: [{$set: {a: [1, 2, 3, 4, 5]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, 4, 5]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Case where we make an array shorter.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, 2, 3]},
    pipeline: [{$set: {a: [1, 2]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, 2]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Change element of array AND shorten it
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, {b: 10}, 3]},
    pipeline: [{$set: {a: [1, {b: 9}]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, {b: 9}]},
    expectedOplogEntry: kExpectDeltaEntry
});

// Remove element from the middle of an array. Should still use a delta, and only rewrite the last
// parts of the array.
id = generateId();
testUpdateReplicates({
    preImage: {_id: id, padding: kGiantStr, a: [1, 2, 999, 3, 4]},
    pipeline: [{$set: {a: [1, 2, 3, 4]}}],
    postImage: {_id: id, padding: kGiantStr, a: [1, 2, 3, 4]},
    expectedOplogEntry: kExpectDeltaEntry
});

function generateDeepObj(depth, maxDepth, value) {
    return {
        "padding": kGiantStr,
        "subObj": (depth >= maxDepth) ? value : generateDeepObj(depth + 1, maxDepth, value)
    };
}

// Verify that diffing the deepest objects allowed by the js client, can produce a delta op-log
// entries.
id = generateId();
let path = "subObj.".repeat(146) + "subObj";
testUpdateReplicates({
    preImage: Object.assign({_id: id}, generateDeepObj(1, 147, 1)),
    pipeline: [{$set: {[path]: 2}}],
    postImage: Object.assign({_id: id}, generateDeepObj(1, 147, 2)),
    expectedOplogEntry: kExpectDeltaEntry
});

// Don't drop any collections. At the end we want the DBHash checker will make sure there's no
// corruption.

rst.stopSet();
})();
