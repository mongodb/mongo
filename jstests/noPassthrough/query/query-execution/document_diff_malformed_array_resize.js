/**
 * Tests that a malformed array diff with an update index exceeding the resize
 * value produces an error instead of crashing the server.
 */
const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 0, arr: [0, 1, 2]}));

// Malformed diff: resize array to 3 elements but update at index 7.
const malformedDiff = {
    $v: NumberInt(2),
    diff: {
        sarr: {
            a: true,
            l: NumberInt(3),
            u7: 999,
        },
    },
};

assert.commandFailedWithCode(
    db.runCommand({
        update: coll.getName(),
        updates: [
            {
                q: {_id: 0},
                u: [{$_internalApplyOplogUpdate: {oplogUpdate: malformedDiff}}],
            },
        ],
    }),
    [12495900, 12495901],
);

assert.eq(coll.findOne({_id: 0}).arr, [0, 1, 2]);

// The same malformed diff via applyOps should also fail safely.
assert.commandFailedWithCode(
    db.adminCommand({
        applyOps: [
            {
                op: "u",
                ns: coll.getFullName(),
                o2: {_id: 0},
                o: {$v: NumberInt(2), diff: {sarr: {a: true, l: NumberInt(3), u7: 999}}},
            },
        ],
    }),
    [12495900, 12495901],
);

assert.eq(coll.findOne({_id: 0}).arr, [0, 1, 2]);

MongoRunner.stopMongod(conn);
