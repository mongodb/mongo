/**
 * Test that $** indexes obey collation rules for document values, while the virtual $_path
 * components stored alongside these values in the index always use simple binary comparison.
 *
 * We require that collections are unsharded, since we perform queries which we expect to be
 * covered.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 * ]
 */
import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";
import {getPlanStages, getWinningPlanFromExplain, isIndexOnly} from "jstests/libs/query/analyze_plan.js";

const assertArrayEq = (l, r) => assert(arrayEq(l, r));

// Create the collection and assign it a default case-insensitive collation.
let coll = assertDropAndRecreateCollection(db, "wildcard_collation", {collation: {locale: "en_US", strength: 1}});

// Extracts the winning plan for the given query and projection from the explain output.
const winningPlan = (query, proj) =>
    FixtureHelpers.isMongos(db)
        ? getWinningPlanFromExplain(getWinningPlanFromExplain(coll.find(query, proj).explain().queryPlanner).shards[0])
        : getWinningPlanFromExplain(coll.find(query, proj).explain().queryPlanner);

// Runs the given query and confirms that: (1) the $** was used to answer the query, (2) the
// results produced by the $** index match the given 'expectedResults', and (3) the same output
// is produced by a COLLSCAN with the same collation.
function assertWildcardIndexAnswersQuery(query, expectedResults, projection) {
    // Verify that the $** index can answer this query.
    const ixScans = getPlanStages(winningPlan(query, projection || {_id: 0}), "IXSCAN");
    assert.gt(ixScans.length, 0, tojson(coll.find(query).explain()));
    ixScans.forEach((ixScan) => assert(ixScan.keyPattern.$_path));

    // Assert that the $** index produces the expected results, and that these are the same
    // as those produced by a COLLSCAN with the same collation.
    const wildcardResults = coll.find(query, projection || {_id: 0}).toArray();
    assertArrayEq(wildcardResults, expectedResults);
    assertArrayEq(
        wildcardResults,
        coll
            .find(query, projection || {_id: 0})
            .collation({locale: "en_US", strength: 1})
            .hint({$natural: 1})
            .toArray(),
    );
}

// Confirms that the index matching the given keyPattern has the specified collation.
function assertIndexHasCollation(keyPattern, collation) {
    let indexSpecs = coll.getIndexes();
    let found = IndexCatalogHelpers.findByKeyPattern(indexSpecs, keyPattern, collation);
    assert.neq(
        null,
        found,
        "Index with key pattern " +
            tojson(keyPattern) +
            " and collation " +
            tojson(collation) +
            " not found: " +
            tojson(indexSpecs),
    );
}

const wildcardIndexes = [{keyPattern: {"$**": 1}}, {keyPattern: {"$**": 1, b: 1}, wildcardProjection: {b: 0}}];

for (const indexSpec of wildcardIndexes) {
    const option = {};
    if (indexSpec.wildcardProjection) {
        option["wildcardProjection"] = indexSpec.wildcardProjection;
    }
    assert.commandWorked(coll.createIndex(indexSpec.keyPattern, option));

    // Confirm that the $** index inherits the collection's default collation.
    assertIndexHasCollation(indexSpec.keyPattern, {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 1,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    });

    // Insert a series of documents whose fieldnames and values differ only by case.
    assert.commandWorked(coll.insert({a: {b: "string", c: "STRING"}, d: "sTrInG", e: 5}));
    assert.commandWorked(coll.insert({a: {b: "STRING", c: "string"}, d: "StRiNg", e: 5}));
    assert.commandWorked(coll.insert({A: {B: "string", C: "STRING"}, d: "sTrInG", E: 5}));
    assert.commandWorked(coll.insert({A: {B: "STRING", C: "string"}, d: "StRiNg", E: 5}));

    // Confirm that only the document's values adhere to the case-insensitive collation. The field
    // paths, which are also present in the $** index keys, are evaluated using simple binary
    // comparison; so for instance, path "a.b" does *not* match path "A.B".
    assertWildcardIndexAnswersQuery({"a.b": "string"}, [
        {a: {b: "string", c: "STRING"}, d: "sTrInG", e: 5},
        {a: {b: "STRING", c: "string"}, d: "StRiNg", e: 5},
    ]);
    assertWildcardIndexAnswersQuery({"A.B": "string"}, [
        {A: {B: "string", C: "STRING"}, d: "sTrInG", E: 5},
        {A: {B: "STRING", C: "string"}, d: "StRiNg", E: 5},
    ]);

    // All documents in the collection are returned if we query over both upper- and lower-case
    // fieldnames, or when the fieldname has a consistent case across all documents.
    const allDocs = coll.find({}, {_id: 0}).toArray();
    assertWildcardIndexAnswersQuery({$or: [{"a.c": "string"}, {"A.C": "string"}]}, allDocs);
    assertWildcardIndexAnswersQuery({d: "string"}, allDocs);

    // Confirm that the $** index also differentiates between upper and lower fieldname case when
    // querying fields which do not contain string values.
    assertWildcardIndexAnswersQuery({e: 5}, [
        {a: {b: "string", c: "STRING"}, d: "sTrInG", e: 5},
        {a: {b: "STRING", c: "string"}, d: "StRiNg", e: 5},
    ]);
    assertWildcardIndexAnswersQuery({E: 5}, [
        {A: {B: "string", C: "STRING"}, d: "sTrInG", E: 5},
        {A: {B: "STRING", C: "string"}, d: "StRiNg", E: 5},
    ]);

    // Confirm that the $** index produces a covered plan for a query on non-string, non-object,
    // non-array values.
    assert(isIndexOnly(coll.getDB(), winningPlan({e: 5}, {_id: 0, e: 1})));
    assert(isIndexOnly(coll.getDB(), winningPlan({E: 5}, {_id: 0, E: 1})));

    // Confirm that the $** index differentiates fieldname case when attempting to cover.
    assert(!isIndexOnly(coll.getDB(), winningPlan({e: 5}, {_id: 0, E: 1})));
    assert(!isIndexOnly(coll.getDB(), winningPlan({E: 5}, {_id: 0, e: 1})));

    coll = assertDropAndRecreateCollection(db, "wildcard_collation", {collation: {locale: "en_US", strength: 1}});
}
