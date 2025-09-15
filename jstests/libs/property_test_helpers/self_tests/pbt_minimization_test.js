/**
 * Self-test for our PBT infrastructure. Asserts that shrinking (minimization) works properly.
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const seed = 4;

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