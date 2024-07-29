/**
 * Given a command and batch size, runs the command and enough getMores to exhaust the cursor,
 * returning all of the results.
 */
export function exhaustFindCursorAndReturnResults(db, cmd) {
    const namespace = cmd.find;

    const initialResult = assert.commandWorked(db.runCommand(cmd));
    let cursor = initialResult.cursor;
    let allResults = cursor.firstBatch;

    while (cursor.id != 0) {
        const getMore = {getMore: cursor.id, collection: namespace};
        const getMoreResult = assert.commandWorked(db.runCommand(getMore));
        cursor = getMoreResult.cursor;
        allResults = allResults.concat(cursor.nextBatch);
    }

    return allResults;
}
