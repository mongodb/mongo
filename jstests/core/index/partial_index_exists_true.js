/**
 * Tests the behavior of $ne and $nin queries that check for null in the presence of partial index
 * with an {$exists: true} partial filter expresion. This test suite evaluates and documents the
 * differences in behavior between queries and partial indexes on top-level fields versus nested
 * (dotted path) fields.
 *
 * @tags: [
 *    # the test conflicts with hidden wildcard indexes
 *    assumes_no_implicit_index_creation,
 *    # We will have different index usage behavior until we backport SERVER-36635.
 *    multiversion_incompatible,
 *    does_not_support_stepdowns
 * ]
 */
import {isCollscan, isIxscan} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];

const getDisableDottedPathIsSubsetOfExistsTrue =
    db.adminCommand({getParameter: 1, internalQueryPlannerDisableDottedPathIsSubsetOfExistsTrue: 1})
        .internalQueryPlannerDisableDottedPathIsSubsetOfExistsTrue;

/**
 * Tests the behavior of a query on a document both:
 * 1) Without any secondary indexes.
 * 2) With a secondary partial index described by 'indexSpec' and 'partialFilterExpression'.
 * - inputDoc: The document to query against.
 * - query: The query to be tested.
 * - indexSpec: The specification of the index to be created.
 * - partialFilterExpression: The partial filter expression for the index.
 * - useIndex: A boolean indicating if the index is expected to be used.
 * - expectEmptyRes: A boolean indicating if the query is expected to return an empty result.
 */
function testPartialFilterExpression(
    inputDoc, query, indexSpec, partialFilterExpression, useIndex, expectEmptyRes) {
    const doc = Object.assign({_id: 0}, inputDoc);

    jsTestLog(`Testing query ${tojson(query)} against document ${tojson(doc)}. 
      Checking consistency of results with and without the index ${tojson(indexSpec)} 
      using partialFilterExpression ${tojson(partialFilterExpression)}. 
      Expect index usage: ${useIndex}, Expect empty result: ${expectEmptyRes}`);

    coll.drop();
    // We need to test the documents in isolation. Documents that have an array or embedded document
    // will make the index multikey and hence, alter the index usage.
    assert.commandWorked(coll.insert(doc));

    // Either an empty result set or the single doc in the collection.
    const expectedRes = expectEmptyRes ? [] : [doc];

    // Check results without an index.
    let actualRes = coll.find(query).toArray();
    assert.sameMembers(actualRes, expectedRes);

    // Now confirm that the partial index does not change the result at all.
    assert.commandWorked(coll.createIndex(indexSpec, partialFilterExpression));

    actualRes = coll.find(query).toArray();
    assert.sameMembers(actualRes, expectedRes);

    // Check that the explain shows expected index usage.
    const actualExplain = coll.find(query).explain();
    if (useIndex) {
        assert(isIxscan(db, actualExplain));
    } else {
        assert(isCollscan(db, actualExplain));
    }
}

// Testing these cases to show that top-level fields don't have the issue
// where {<field>: {$ne: null}} is incompatible with {<field>: {$exists: true}} index.
// Documents where {a: {$ne: null}} is true. Index and query on field "a".
(function testANeNull() {
    const query = {a: {$ne: null}};
    const indexSpec = {a: 1};
    const pfe = {partialFilterExpression: {a: {$exists: true}}};

    // Documents expected to match the query ($ne: null) and can use the given index.
    const indexUsableNonEmptyResults =
        [{a: undefined}, {a: 1}, {a: {}}, {a: {b: null}}, {a: {c: 1}}];

    indexUsableNonEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ true, /* expectEmptyRes */ false);
    });

    // Documents expected to match the query ($ne: null) but the index is not usable due to
    // multikey.
    const indexNotUsableNonEmptyResults = [
        {a: []},
        {a: [1]},
        {a: [1, 2, 3]},
        {a: [{}]},
        {a: [[]]},
        {a: [1, 2, 3]},
        {a: [1, "2", {}]}
    ];

    indexNotUsableNonEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ false, /* expectEmptyRes */ false);
    });

    // Documents that match {a: {$eq: null}} and thus are not expected to match the query ($ne:
    // null) but can still use the index.
    const indexUsableEmptyResults = [{a: null}, {b: 1}];

    indexUsableEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ true, /* expectEmptyRes */ true);
    });

    // Documents not expected to match the query ($eq: null results) and the index is not usable
    // (multikey).
    const indexNotUsableEmptyResults = [{a: [null]}, {a: [1, null]}];

    indexNotUsableEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ false, /* expectEmptyRes */ true);
    });
})();

// Testing cases to ensure {'a.b': {$ne: null}} returns correct results with and without the
// presence of an index. We expect the query with the dotted path to not utilize the given index.
// See SERVER-36635 for more context on this behavior.
(function testABNeNull() {
    // Skip tests if query knob is disabled.
    if (!getDisableDottedPathIsSubsetOfExistsTrue) {
        return;
    }
    const query = {"a.b": {$ne: null}};
    const indexSpec = {"a.b": 1};
    const pfe = {partialFilterExpression: {"a.b": {$exists: true}}};

    // These documents match {"a.b": {$exists: true}} and {"a.b": {$ne: null}}.
    // Thus the query can safely use the partial index to return these documents.
    // Queries on collections with documents of this shape will no longer use the partial index as a
    // result of changes in SERVER-36635.
    const docCanUsePartialIndex = [{a: {b: 1}}, {a: [{b: 1}]}];

    // These documents do not match {"a.b": {$exists: true}} but do match {"a.b": {$ne: null}}.
    // Thus the query could not have used the partial index to return these documents.
    // Queries on collections with documents of this shape were incorrectly using the partial index
    // prior to SERVER-36635
    const docsCannotUsePartialIndex = [{a: []}, {a: [1]}, {a: [null]}, {a: [[]]}];

    // Documents expected to match {$ne: null} on 'a.b' and index is not usable.
    const indexNotUsableNonEmptyResults = [...docCanUsePartialIndex, ...docsCannotUsePartialIndex];
    indexNotUsableNonEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ false, /* expectEmptyRes */ false);
    });

    // Documents that match {'a.b': {$eq: null}} and thus are not expected to match {$ne: null} on
    // 'a.b' and index is not usable.
    const indexNotUsableEmptyResults = [
        {a: 1},
        {c: 1},
        {a: null},
        {a: [{}]},
        {a: [{b: null}]},
        {a: [1, {b: null}]},
        {a: {}},
        {a: {c: 1}}
    ];

    indexNotUsableEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ false, /* expectEmptyRes */ true);
    });

    // The tests below use an index key path that is not a prefix of the partialFilterExpression
    // keypath. The set of documents and the expected outputs are the same as the tests above.
    const queryWithX = {"x": 2, 'a.b': {$ne: null}};
    const indexSpecWithX = {x: 1};

    const indexNotUsableNonEmptyResultsWithX = indexNotUsableNonEmptyResults.map(doc => {
        return {...doc, x: 2};
    });
    indexNotUsableNonEmptyResultsWithX.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpecWithX, pfe, /* useIndex */ false, /* expectEmptyRes */ false);
    });

    const indexNotUsableEmptyResultsWithX = indexNotUsableEmptyResults.map(doc => {
        return {...doc, x: 2};
    });
    indexNotUsableEmptyResultsWithX.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpecWithX, pfe, /* useIndex */ false, /* expectEmptyRes */ true);
    });
})();

// Testing cases to ensure $nin with a null returns correct results with and without an index on a
// top-level field. We expect the query on a top-level field to utilize the given index.
(function testANinNull() {
    // $nin array must have at least 2 elements to prevent it from being desugared to $ne.
    const query = {"a": {$nin: [null, 100]}};
    const indexSpec = {"a": 1};
    const pfe = {partialFilterExpression: {"a": {$exists: true}}};

    // Documents that match the query and can use the index as it is on a top-level field.
    const indexUsableNonEmptyResults =
        [{a: undefined}, {a: 1}, {a: {}}, {a: {b: null}}, {a: {c: 1}}];

    indexUsableNonEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ true, /* expectEmptyRes */ false);
    });
})();

// Testing cases to ensure $nin with a null returns correct results with and without an index.
// We expect the query with the dotted path to not utilize the given index. See SERVER-36635 for
// more context on this behavior.
(function testABNinNull() {
    // Skip tests if query knob is disabled.
    if (!getDisableDottedPathIsSubsetOfExistsTrue) {
        return;
    }
    // $nin array must be at least 2 elements to prevent it from being parsed as $ne
    const query = {"a.b": {$nin: [null, 100]}};
    const indexSpec = {"a.b": 1};
    const pfe = {partialFilterExpression: {"a.b": {$exists: true}}};

    // These documents match {"a.b": {$exists: true}} and {"a.b": {$nin: [null, ...]}}.
    // Thus the query can safely use the partial index to return these documents.
    // Queries on collections with documents of this shape will no longer use the partial index as a
    // result of changes in SERVER-36635.
    const docsCanUsePartialIndex = [{a: {b: 1}}, {a: [{b: 1}]}];

    // These documents do not match {"a.b": {$exists: true}} but do match {"a.b": {$nin: [null,
    // ...]}}. Thus the query could not have used the partial index to return these documents.
    // Queries on collections with documents of this shape were incorrectly using the partial index
    // prior to SERVER-36635
    const docsCannotUsePartialIndex = [{a: []}, {a: [1]}, {a: [null]}, {a: [[]]}];

    // Documents expected to match {$nin: [null, 100]} on 'a.b', index not usable.
    const indexNotUsableNonEmptyResults = [...docsCanUsePartialIndex, ...docsCannotUsePartialIndex];

    indexNotUsableNonEmptyResults.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpec, pfe, /* useIndex */ false, /* expectEmptyRes */ false);
    });

    // The tests below use an index key path that is not a prefix of the partialFilterExpression
    // keypath. The set of documents and the expected outputs are the same as the tests above.
    const queryWithX = {"x": 2, 'a.b': {$nin: [null, 100]}};
    const indexSpecWithX = {x: 1};
    const indexNotUsableNonEmptyResultsWithX = indexNotUsableNonEmptyResults.map(doc => {
        return {
            ...doc,
            x: 2,
        };
    });
    indexNotUsableNonEmptyResultsWithX.forEach(doc => {
        testPartialFilterExpression(
            doc, query, indexSpecWithX, pfe, /* useIndex */ false, /* expectEmptyRes */ false);
    });
})();
