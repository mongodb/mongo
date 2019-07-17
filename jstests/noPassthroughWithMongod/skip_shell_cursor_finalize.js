// Test skipShellCursorFinalize shell option.

(function() {
"use strict";
const coll = db.skip_shell_cursor_finalize;
coll.drop();

for (let i = 0; i < 3; i++) {
    assert.writeOK(coll.insert({_id: i}));
}

function checkShellCursorFinalize(skip = true) {
    const coll = db.skip_shell_cursor_finalize;
    function getIdleCursors() {
        const adminDB = db.getSiblingDB("admin");
        return adminDB
            .aggregate([
                {$currentOp: {idleCursors: true}},
                {$match: {$and: [{type: "idleCursor"}, {ns: coll.toString()}]}}
            ])
            .toArray();
    }

    let cursor = coll.find({}).batchSize(2);
    assert(cursor.next(), "cursor should have next doc");
    assert(!cursor.isClosed(), "cursor should not be closed");
    assert.eq(getIdleCursors().length, 1);

    // Set the cursor to null so that it can be garbage collected.
    cursor = null;
    gc();

    // The idle cursor should remain the same if cursor finalize is skipped, otherwise there
    // should be no idle cursor on this collection.
    assert.eq(getIdleCursors().length, skip ? 1 : 0);
}

// Start a shell without the skipShellCursorFinalize shell option.
let awaitShell = startParallelShell("(" + checkShellCursorFinalize + ")(false);");
awaitShell();

// Start a shell with the skipShellCursorFinalize shell option.
awaitShell = startParallelShell(checkShellCursorFinalize,
                                undefined,
                                false,
                                "--setShellParameter",
                                "skipShellCursorFinalize=true");
awaitShell();
})();
