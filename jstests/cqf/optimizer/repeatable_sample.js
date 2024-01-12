/**
 * Tests that cardinality estimates for different predicates in the same query are all
 * based on the same sample of documents. (When 'internalCascadesOptimizerRepeatableSample' is
 * enabled.)
 */
import {navigateToPath, navigateToPlanPath, runWithParams} from 'jstests/libs/optimizer_utils.js';

const coll = db.cqf_repeatable_sample;
coll.drop();

// Insert some documents where two fields have identical values.
const numDocs = 10000;
assert.commandWorked(coll.insert(Array.from({length: numDocs}, (_, i) => ({a: i, b: i}))));
// Create indexes on both fields, because we only sample indexed fields.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// To avoid having this test pass trivially, we want a predicate that is
// unlikely to be estimated to have a selectivity of 0 or 1. So choose
// a predicate whose true selectivity is 0.5: its estimated selectivity
// will only be 0 or 1 if the sampled documents are exclusively from the
// lower or upper half of the collection.
const predicate = {
    $lt: numDocs / 2
};

// Run a query where the true selectivity of both predicates is identical.
// Because the server draws only one sample of documents, and reuses it for
// both estimates, we expect the two estimates to be identical.
//
// If the server were drawing two separate samples, it would still be possible
// for them to have the same estimate. But it appears to be unlikely, because
// as long as at least one chunk straddles the midpoint of the collection,
// the estimates are very granular.
const explain = runWithParams(
    [
        {key: 'internalCascadesOptimizerSampleChunks', value: 10},
        {key: 'internalCascadesOptimizerRepeatableSample', value: true},
    ],
    () => coll.find({a: predicate, b: predicate}).explain());

// We don't care what the plan is, we just want the 'requirementCEs' property for the whole plan.
// Apparently this is not available on the Root node, but it is available on the topmost non-Root
// node.
const root = navigateToPlanPath(explain, "");
const node = root.child;
const requirementCEs =
    navigateToPath(node, 'properties.logicalProperties.cardinalityEstimate.1.requirementCEs');
assert.eq(requirementCEs.length,
          2,
          `Expected two requirementCEs: one per predicate, but got ${
              tojson(requirementCEs)}. The overall plan was ${tojson(root)}`);
assert.eq(requirementCEs[0].mode, "sampling", `Expected both requirementCEs use sampling`);
assert.eq(requirementCEs[1].mode, "sampling", `Expected both requirementCEs use sampling`);
assert.eq(requirementCEs[0].ce, requirementCEs[1].ce, `Expected the two estimates to be identical`);
