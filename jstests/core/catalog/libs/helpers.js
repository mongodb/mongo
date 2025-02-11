/*
 * Tiny library of functions supporting the test cases around management of cluster metadata.
 */

// Wrapper for the listCollection user command.
export function getListCollectionsCursor(connToDb, options = {}, subsequentBatchSize) {
    // SERVER-81986: After SERVER-72229, there can be an implicitly created collection named
    // "system.profile" during an FCV upgrade. The presence of this collection can break some
    // assertions in this test. This makes sure that we filter out that collection.
    if (typeof options == "object" && options.filter === undefined) {
        options.filter = {name: {$ne: "system.profile"}};
    }

    return new DBCommandCursor(
        connToDb, connToDb.runCommand("listCollections", options), subsequentBatchSize);
}

// Exhausts the passed in cursor and counts the number of items matching the specified predicate.
export function cursorCountMatching(cursor, pred) {
    return cursor.toArray().filter(pred).length;
}
