/**
 * Tests that IDHACK (and thus, the express path) works on queries of the shape {_id: {$eq: 123}}.
 *
 * @tags: [
 *   requires_fcv_81,
 *   # "Explain for the aggregate command cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 *   # setParameter not permitted with security tokens
 *   not_allowed_with_signed_security_token,
 *   # "Refusing to run a test that issues commands that may return different values after a
 *   # failover"
 *   does_not_support_stepdowns,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {isExpress, isIdhack, isIdhackOrExpress} from "jstests/libs/query/analyze_plan.js";

const testDB = db.getSiblingDB("express_id_eq");
assert.commandWorked(testDB.dropDatabase());

const caseInsensitiveCollation = {
    locale: "en_US",
    strength: 1
};
const docs = [{_id: 123, a: 1}, {_id: "str", a: 1}, {_id: {}}, {_id: {a: 1}}];

function assertUsesExpress(coll, filter, expectedResults) {
    function assertOnResultAndExplain(result, explain) {
        assertArrayEq({
            actual: result,
            expected: expectedResults,
            extraErrorMsg:
                "Result set comparison failed for find(" + filter + "). Explain: " + tojson(explain)
        });

        assert(isExpress(testDB, explain),
               "Expected the query to use express. Explain: " + tojson(explain));
    }

    // Find
    {
        // If the collection has a collation, make sure to set the collation for the query so that
        // we use the express path.
        let findCollation = {};
        if (coll.getName() == "collection_with_collation") {
            findCollation = caseInsensitiveCollation;
        }

        const result = coll.find(filter).collation(findCollation).toArray();
        const explain = coll.find(filter).collation(findCollation).explain();

        assertOnResultAndExplain(result, explain);

        // Before the changes to expand IDHACK to allow queries like {_id: {$eq: 123}}, that kind of
        // query went through the express path as one that is a query on a user index. Now it is
        // correctly considered to be on the id index. The query knob
        // 'internalQueryDisableSingleFieldExpressExecutor' only disables the express path for
        // queries on a user index, so disabling the knob and running the query again should show
        // that we still use the express path.
        testDB.adminCommand(
            {setParameter: 1, internalQueryDisableSingleFieldExpressExecutor: true});
        const explainAfterKnobDisabled = coll.find(filter).collation(findCollation).explain();
        assert(
            isExpress(testDB, explainAfterKnobDisabled),
            "Expected the query to use express after disabling 'internalQueryDisableSingleFieldExpressExecutor'. Explain: " +
                tojson(explainAfterKnobDisabled));
        testDB.adminCommand(
            {setParameter: 1, internalQueryDisableSingleFieldExpressExecutor: false});
    }

    // Agg, to test pushdown to find.
    {
        // If the collection has a collation, make sure to set the collation for the query so that
        // we use the express path.
        const aggOptions = {};
        if (coll.getName() == "collection_with_collation") {
            aggOptions.collation = caseInsensitiveCollation;
        }

        const result = coll.aggregate([{$match: filter}], aggOptions).toArray();
        const explain = coll.explain().aggregate([{$match: filter}], aggOptions);

        assertOnResultAndExplain(result, explain);

        // See comment above.
        testDB.adminCommand(
            {setParameter: 1, internalQueryDisableSingleFieldExpressExecutor: true});
        const explainAfterKnobDisabled = coll.explain().aggregate([{$match: filter}], aggOptions);
        assert(
            isExpress(testDB, explainAfterKnobDisabled),
            "Expected the query to use express after disabling 'internalQueryDisableSingleFieldExpressExecutor'. Explain: " +
                tojson(explainAfterKnobDisabled));
        testDB.adminCommand(
            {setParameter: 1, internalQueryDisableSingleFieldExpressExecutor: false});
    }
}

function assertUsesIdHack(coll, filter, projection, expectedResults) {
    const result = coll.find(filter, projection).toArray();
    const explain = coll.find(filter, projection).explain();

    assertArrayEq({
        actual: result,
        expected: expectedResults,
        extraErrorMsg: "Result set comparison failed for find(" + filter + ", " + projection +
            "). Explain: " + tojson(explain)
    });

    // Assert that the plan is IDHACK, and not express.
    assert(isIdhack(testDB, explain),
           "Expected the query to use IDHACK. Explain: " + tojson(explain));
}

function runTests(collection, isSimpleCollation) {
    // Ensure that a query like {_id: {$eq: 123}} (which is equivalent to {_id: 123}) correctly goes
    // through the express path.
    assertUsesExpress(collection, {_id: 123}, [docs[0]]);
    assertUsesExpress(collection, {_id: {$eq: 123}}, [docs[0]]);

    // Ensure that a query like {_id: {$in: [123]}} (which gets optimized to {_id: 123})
    // correctly goes through the express path.
    assertUsesExpress(collection, {_id: {$in: [123]}}, [docs[0]]);

    // Test that equality to empty objects works as expected.
    assertUsesExpress(collection, {_id: {}}, [docs[2]]);
    assertUsesExpress(collection, {_id: {$eq: {}}}, [docs[2]]);
    assertUsesExpress(collection, {_id: {$in: [{}]}}, [docs[2]]);

    // Test that equality to non-empty objects works as expected.
    assertUsesExpress(collection, {_id: {a: 1}}, [docs[3]]);
    assertUsesExpress(collection, {_id: {$eq: {a: 1}}}, [docs[3]]);
    assertUsesExpress(collection, {_id: {$in: [{a: 1}]}}, [docs[3]]);

    // // Assert that the same queries as above use IDHACK in the presence of a complex projection
    // // (which is not supported by the express path).
    const complexProj = {_id: 1, "foo.bar": 0};
    assertUsesIdHack(collection, {_id: 123}, complexProj, [docs[0]]);
    assertUsesIdHack(collection, {_id: {$eq: 123}}, complexProj, [docs[0]]);
    assertUsesIdHack(collection, {_id: {$in: [123]}}, complexProj, [docs[0]]);
    assertUsesIdHack(collection, {_id: {}}, complexProj, [docs[2]]);
    assertUsesIdHack(collection, {_id: {$eq: {}}}, complexProj, [docs[2]]);
    assertUsesIdHack(collection, {_id: {$in: [{}]}}, complexProj, [docs[2]]);
    assertUsesIdHack(collection, {_id: {a: 1}}, complexProj, [docs[3]]);
    assertUsesIdHack(collection, {_id: {$eq: {a: 1}}}, complexProj, [docs[3]]);
    assertUsesIdHack(collection, {_id: {$in: [{a: 1}]}}, complexProj, [docs[3]]);

    // Same queries as above, just testing a query that should be affected by collation.
    // Since the collation is case-insensitive, we should be able to find the document whose
    // '_id' field is 'str' by querying for 'STR'.
    assertUsesExpress(collection, {_id: "STR"}, isSimpleCollation ? [] : [docs[1]]);
    assertUsesExpress(collection, {_id: {$eq: "STR"}}, isSimpleCollation ? [] : [docs[1]]);
    assertUsesExpress(collection, {_id: {$in: ["STR"]}}, isSimpleCollation ? [] : [docs[1]]);

    assertUsesIdHack(
        collection, {_id: "STR"}, {_id: 1, "foo.bar": 0}, isSimpleCollation ? [] : [docs[1]]);
    assertUsesIdHack(
        collection, {_id: {$eq: "STR"}}, complexProj, isSimpleCollation ? [] : [docs[1]]);
    assertUsesIdHack(
        collection, {_id: {$in: ["STR"]}}, complexProj, isSimpleCollation ? [] : [docs[1]]);

    // Assert that equality to null does not use express because 'null' isn't an exact bounds
    // generating type.
    let explain = collection.find({_id: {$eq: null}}).explain();
    assert(!isExpress(testDB, explain),
           "Expected the query to not express. Explain: " + tojson(explain));

    // Assert that a find that contains an express-eligible predicate but has other stipulations
    // does not go through either IDHACK or express. If the collection is a clustered collection on
    // '_id', then we will skip this assertion since 'isIdHack' returns true if the winning plan was
    // 'CLUSTERED_IXSCAN'.
    var isClustered = ClusteredCollectionUtil.areAllCollectionsClustered(testDB);
    if (collection.getName() != "clustered_collection" && !isClustered) {
        explain = collection.find({_id: {$eq: 1, $lt: 4}}).explain();
        assert(!isIdhackOrExpress(testDB, explain),
               "Expected the query to not express or IDHACK. Explain: " + tojson(explain));
    }
}

// Collection without collation.
assert.commandWorked(testDB.createCollection("no_collation_collection"));
const coll = testDB.no_collation_collection;
coll.remove({});
assert.commandWorked(coll.insert(docs));
runTests(coll, true /* isSimpleCollation */);

// Collation of the collection is non-simple (and thus any indexes, including the _id index, will
// automatically also have that collation).
assert.commandWorked(
    testDB.createCollection("collection_with_collation", {collation: caseInsensitiveCollation}));
const collWithCollation = testDB.collection_with_collation;
collWithCollation.remove({});
assert.commandWorked(collWithCollation.insert(docs));
runTests(collWithCollation, false /* isSimpleCollation */);

// Clustered collection.
assert.commandWorked(testDB.createCollection("clustered_collection",
                                             {clusteredIndex: {"key": {_id: 1}, "unique": true}}));
const clusteredColl = testDB.clustered_collection;
clusteredColl.remove({});
assert.commandWorked(clusteredColl.insert(docs));
runTests(clusteredColl, true /* isSimpleCollation */);
