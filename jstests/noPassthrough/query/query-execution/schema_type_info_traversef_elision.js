/**
 * Verifies that registering SchemaTypeInfo (inferred from a collection's document validator) on the
 * ExpressionContext alongside PathArrayness does not regress the existing "path cannot be an array"
 * SBE optimization (traverseF elision) that PathArrayness alone already provides via index metadata.
 *
 * Note: a plain '$type' validator predicate uses MongoDB's default array-traversal matching
 * semantics, so it can match a document where the field holds an array of the given type; it can
 * therefore never *by itself* prove a field is not an array (only 'constraint' + an index that is
 * known non-multikey can). This test instead confirms the two signals combine correctly: an
 * index-derived "cannot be array" result is preserved even when a validator is also present.
 *
 * @tags: [requires_sbe]
 */
import {getQueryPlanners, getEngine} from "jstests/libs/query/analyze_plan.js";

// Use the default query framework configuration ('trySbeRestricted'). A '$group' with a '$match'
// prefix is lowered to SBE under that default, so we exercise the same traverseF elision path
// without forcing SBE via 'trySbeEngine'.
const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        featureFlagQueryTypeInference: true,
    },
});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
const coll = db[jsTestName()];

function stagesForFilter(createOptions, withIndex, filter) {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), createOptions));
    if (withIndex) {
        assert.commandWorked(coll.createIndex({a: 1}));
    }
    assert.commandWorked(coll.insert({_id: 0, a: "value"}));

    // The '$group' stage forces the pipeline (including the '$match' prefix) into SBE under the
    // default 'trySbeRestricted' configuration.
    const pipeline = [{$match: filter}, {$group: {_id: null, c: {$sum: 1}}}];
    const explain = coll.explain().aggregate(pipeline, {hint: withIndex ? {a: 1} : {$natural: 1}});
    assert.eq(getEngine(explain), "sbe", "Expected SBE engine: " + tojson(explain));
    return getQueryPlanners(explain)[0].winningPlan.slotBasedPlan.stages;
}

const validatorOptions = {validator: {a: {$type: "string"}}, validationLevel: "constraint"};

// With a non-multikey index on 'a' and a 'constraint' validator both present, PathArrayness's
// index-derived "cannot be array" result must still cause traverseF to be elided.
let stages = stagesForFilter(validatorOptions, /*withIndex=*/ true, {a: {$type: "string"}});
assert(
    !stages.includes("traverseF"),
    "Expected traverseF elided via index-derived PathArrayness even with a validator present: " +
        stages,
);

// Without an index, neither PathArrayness nor the validator (whose '$type' predicate allows an
// array of matching elements) can rule out an array, so traverseF must still be generated. This
// confirms SchemaTypeInfo's conservative default doesn't regress correctness.
stages = stagesForFilter(validatorOptions, /*withIndex=*/ false, {a: {$type: "string"}});
assert(
    stages.includes("traverseF"),
    "Expected traverseF without index-derived arrayness info: " + stages,
);

MongoRunner.stopMongod(conn);
