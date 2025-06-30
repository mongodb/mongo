import {show} from "jstests/libs/golden_test.js";
import {sequentialIds} from "jstests/query_golden/libs/example_data.js";

/**
 * Drops 'coll' and repopulates it with 'docs' and 'indexes'. Sequential _ids are added to
 * documents which do not have _id set.
 */
export function resetCollection(coll, docs, indexes = []) {
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

/**
 * Reduces a query plan in-place to a more compact representation by retaining only the fields
 * that pertain to stage names, filtering and index usage. This representation is suitable for
 * golden tests such as plan_stability.js where we want to record the general shape of the
 * query plan on a single line.
 */
export function trimPlanToStagesAndIndexes(obj) {
    const fieldsToKeep =
        ['stage', 'inputStage', 'inputStages', 'indexName', 'indexBounds', 'filter'];

    if (typeof obj !== 'object' || obj === null) {
        return obj;
    }
    for (let key in obj) {
        if (!Array.isArray(obj) && !fieldsToKeep.includes(key)) {
            delete obj[key];
        } else if (key == "filter") {
            // Preserve the presence of a filter without retaining the actual expression
            obj[key] = true;
        } else {
            if (typeof obj[key] === 'object' && obj[key] !== null && key !== 'indexBounds') {
                trimPlanToStagesAndIndexes(obj[key]);
            }
        }
    }
    return obj;
}

export function padNumber(num) {
    return num.toString().padStart(6, ' ');
}
