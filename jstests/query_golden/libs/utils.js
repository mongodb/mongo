load("jstests/query_golden/libs/example_data.js");

/**
 * Drops 'coll' and repopulates it with 'docs' and 'indexes'. Sequential _ids are added to
 * documents which do not have _id set.
 */
function resetCollection(coll, docs, indexes = []) {
    coll.drop();

    const docsWithIds = sequentialIds(docs);
    jsTestLog("Resetting collection. Inserting docs:");
    show(docsWithIds);

    assert.commandWorked(coll.insert(docsWithIds));
    print(`Collection count: ${coll.find().itcount()}`);

    if (indexes.length > 0) {
        jsTestLog("Creating indexes:");
        show(indexes);
        for (let indexSpec of indexes) {
            assert.commandWorked(coll.createIndex(indexSpec));
        }
    }
}
