// @tags: [requires_replication]
// SERVER-28285 When renameCollection drops the target collection, it should just generate
// a single oplog entry, so we cannot end up in a state where the drop has succeeded, but
// the rename didn't.
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rs = new ReplSetTest({nodes: 1});
rs.startSet();
rs.initiate();

let prim = rs.getPrimary();
let first = prim.getDB("first");
let second = prim.getDB("second");
let local = prim.getDB("local");

// Test both for rename within a database as across databases.
const tests = [
    {
        source: first.x,
        target: first.y,
        expectedOplogEntries: 1,
    },
    {
        source: first.x,
        target: second.x,
        expectedOplogEntries: 4,
    },
];
tests.forEach((test) => {
    test.source.drop();
    assert.commandWorked(test.source.insert({}));
    assert.commandWorked(test.target.insert({}));
    // Other things may be going on in the system; look only at oplog entries affecting the
    // particular databases under test.
    const dbregex = "^(" + test.source.getDB().getName() + ")|(" + test.target.getDB().getName() + ")\\.";

    let ts = local.oplog.rs.find().sort({$natural: -1}).limit(1).next().ts;
    let cmd = {
        renameCollection: test.source.toString(),
        to: test.target.toString(),
        dropTarget: true,
    };
    assert.commandWorked(local.adminCommand(cmd), tojson(cmd));
    let ops = local.oplog.rs
        .find({ts: {$gt: ts}, ns: {"$regex": dbregex}})
        .sort({$natural: 1})
        .toArray();
    assert.eq(
        ops.length,
        test.expectedOplogEntries,
        "renameCollection was supposed to only generate " +
            test.expectedOplogEntries +
            " oplog entries: " +
            tojson(ops),
    );
});
rs.stopSet();
