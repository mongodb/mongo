// Perform a set number of renames from collA to collB and vice versa. This function is to be called
// from a parallel shell, and is useful for simulating executions of functions concurrently with
// collection renames.
function doRenames(dbName, collName, otherName) {
    const repeatRename = 200;
    // Signal to the parent shell that the parallel shell has started.
    assert.writeOK(db.await_data.insert({_id: "signal parent shell"}));
    let renameDB = db.getSiblingDB(dbName);
    for (let i = 0; i < repeatRename; i++) {
        // Rename the collection back and forth.
        assert.commandWorked(renameDB[collName].renameCollection(otherName));
        assert.commandWorked(renameDB[otherName].renameCollection(collName));
    }
    // Signal to the parent shell that the renames have completed.
    assert.writeOK(db.await_data.insert({_id: "rename has ended"}));
}
