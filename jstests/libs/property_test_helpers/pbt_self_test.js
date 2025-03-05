/**
 * Self-tests for our PBT models, to make sure they have intended behavior.
 * The behaviors we check are:
 *   - Our collection model generates an acceptable number of documents on average (>100). This is
 *     essential to the PBTs, because to run meaningful tests and have queries return results, we
 *     need enough documents.
 *   - Shrinking (minimization) of the collection model works as intended.
 */

import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const seed = 4;

function avg(arrOfInts) {
    let sum = 0;
    for (const n of arrOfInts) {
        sum += n;
    }
    return sum / arrOfInts.length;
}

function avgDocumentsIsAbove100(isTS) {
    const collModel = getCollectionModel({isTS});
    const sample = fc.sample(collModel, {seed, numRuns: 1000});

    const avgNumDocs = avg(sample.map(coll => coll.docs.length));
    assert.gt(avgNumDocs, 100);
    jsTestLog('Average number of documents was: ' + avgNumDocs);
}

avgDocumentsIsAbove100(false);
avgDocumentsIsAbove100(true);

/*
 * Fails if we have 1 or more docs, or any indexes.
 * For this property, our minimal counterexample should have exactly 1 doc, and 0 indexes.
 */
function mockProperty(collection) {
    if (collection.docs.length >= 1) {
        return false;
    }
    if (collection.indexes.length > 0) {
        return false;
    }
    return true;
}

const fullyMinimizedDoc = {
    t: new Date(0),
    m: {m1: 0, m2: 0},
    array: 0,
    a: 0,
    b: 0
};

function testShrinking(isTS) {
    const collModel = getCollectionModel({isTS});

    let reporterRan = false;
    function reporter(runDetails) {
        assert(runDetails.failed, runDetails);
        const {isTS, docs, indexes} = runDetails.counterexample[0];
        // Expect 1 doc for full minimization
        assert.eq(docs.length, 1, docs);
        // _ids cannot be fully minimized because of how we assign them out uniquely.
        delete docs[0]._id;
        assert.eq(docs[0], fullyMinimizedDoc, docs);
        // Expect 0 indexes
        assert.eq(indexes.length, 0, indexes);
        reporterRan = true;
    }

    // This property should fail, and our `reporter` will be called, which asserts that we see the
    // minimized counterexample.
    fc.assert(fc.property(collModel, mockProperty), {seed, numRuns: 100, reporter});
    // Assert that the property didn't silently pass.
    assert(reporterRan);
}

testShrinking(false);
testShrinking(true);