/**
 * Test how the semantics of $addFields interacts with a subsequent $match.
 *
 * Intuitively, the $match can be pushed down if it only touches parts of the
 * document that the $addFields left unchanged. However, the behavior is
 * tricky when implicit array traversal is involved: this rewrite would be
 * correct only for some predicates.
 *
 * @tags: [
 *   # $documents is not allowed to be used within a $facet stage
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

/**
 * Runs the stages in both orders: [stage1, stage2] and [stage2, stage1].
 * Returns both outputs, and a readable message showing intermediates.
 */
function runBothOrders({input, stage1, stage2}) {
    const intermediate = db.aggregate([{$documents: input}, stage1]).toArray();
    const output = db.aggregate([{$documents: intermediate}, stage2]).toArray();

    const swappedIntermediate = db.aggregate([{$documents: input}, stage2]).toArray();
    const swappedOutput = db.aggregate([{$documents: swappedIntermediate}, stage1]).toArray();

    const message = `
            ${tojson(input)}
        ${tojson(stage1)}
            ${tojson(intermediate)}
        ${tojson(stage2)}
            ${tojson(output)}
    vs
            ${tojson(input)}
        ${tojson(stage2)}
            ${tojson(swappedIntermediate)}
        ${tojson(stage1)}
            ${tojson(swappedOutput)}
`;
    return {output, swappedOutput, message};
}

/**
 * Test that the behavior of 'stage2' on each input doc is unaffected by 'stage1',
 * and therefore swapping them leaves the final output unchanged.
 */
function testIndependent({inputDoc, stage1, stage2}) {
    jsTestLog(`Test independent stages: ${tojson(stage1)}, ${tojson(stage2)}`);
    const {output, swappedOutput, message} = runBothOrders({input: [inputDoc], stage1, stage2});
    jsTestLog(message);
    assert.eq(
        output, swappedOutput, `Stages are not independent: ${tojson(stage1)}, ${tojson(stage2)}`);
}

/**
 * Test that the behavior of 'stage2' does depend on the changes done by 'stage1',
 * and therefore swapping them is not a correct optimization.
 *
 * Test that running [stage1, stage2] together produces the same result optimized
 * and unoptimized.
 */
function testDependent({inputDoc, stage1, stage2}) {
    jsTestLog(`Test dependent stages: ${tojson(stage1)}, ${tojson(stage2)}`);
    const {output, swappedOutput, message} = runBothOrders({input: [inputDoc], stage1, stage2});
    jsTestLog(message);
    assert.neq(output, swappedOutput, `Input ${tojson([
                   inputDoc
               ])} is not a counterexample for ${tojson(stage1)}, ${tojson(stage2)}`);

    const optimized = db.aggregate([{$documents: [inputDoc]}, stage1, stage2]).toArray();
    assert.eq(optimized, output, `Pipeline must have been optimized incorrectly`);
}

/**
 * Puts {$_internalInhibitOptimization: {}} before every stage to prevent optimizations.
 */
function unoptimized(pipeline) {
    return pipeline.flatMap(stage => [{$_internalInhibitOptimization: {}}, stage]);
}

/**
 * Test that the two pipelines behave the same on the input document.
 *
 * Does nothing to prevent optimizations, so caller may want to use $_internalInhibitOptimization
 * to compare with/without optimization.
 */
function testEquivalent({inputDoc, pipeline1, pipeline2}) {
    jsTestLog(`Test equivalent pipelines: ${tojson(pipeline1)} vs ${tojson(pipeline2)}`);
    const result1 = db.aggregate([{$documents: [inputDoc]}, ...pipeline1]).toArray();
    const result2 = db.aggregate([{$documents: [inputDoc]}, ...pipeline2]).toArray();
    jsTestLog(`
            ${tojson(inputDoc)}
        ${tojson(pipeline1)}
            ${tojson(result1)}
    vs
            ${tojson(inputDoc)}
        ${tojson(pipeline2)}
            ${tojson(result2)}
`);
    assert.eq(result1,
              result2,
              `Pipelines ${tojson(pipeline1)} vs ${tojson(pipeline2)} are not equivalent`);
}

/**
 * Test that the two pipelines are not equivalent: it would be incorrect to rewrite
 * one to the other.
 */
function testNotEquivalent({inputDoc, correct, incorrect}) {
    jsTestLog(`Test non-equivalent pipelines: ${tojson(correct)} vs ${tojson(incorrect)}`);
    const correctResult =
        db.aggregate([{$documents: [inputDoc]}, ...unoptimized(correct)]).toArray();
    const incorrectResult =
        db.aggregate([{$documents: [inputDoc]}, ...unoptimized(incorrect)]).toArray();

    const optimizedResult = db.aggregate([{$documents: [inputDoc]}, ...correct]).toArray();

    jsTestLog({
        inputDoc,

        correct,
        correctResult,

        incorrect,
        incorrectResult,

        optimizedResult,
    });
    assert.neq(correctResult,
               incorrectResult,
               `Input ${tojson(inputDoc)} is not a counterexample for ${tojson(correct)} vs ${
                   tojson(incorrect)}`);
    assert.eq(correctResult,
              optimizedResult,
              `Input ${tojson(inputDoc)} shows that ${tojson(correct)} was incorrectly optimized`);
}

// When the first component of the path is different, then they really are independent paths.
// The $match does not read anything that the $addFields affected.
{
    const stage1 = {$addFields: {x: 5}};
    const stage2 = {$match: {y: null}};
    // Non-array case with missing, scalar, object.
    testIndependent({stage1, stage2, inputDoc: {}});
    testIndependent({stage1, stage2, inputDoc: {x: 0}});
    testIndependent({stage1, stage2, inputDoc: {x: {}}});
    // Arrays containing: no elements, scalar, object.
    testIndependent({stage1, stage2, inputDoc: {x: []}});
    testIndependent({stage1, stage2, inputDoc: {x: [0]}});
    testIndependent({stage1, stage2, inputDoc: {x: [{}]}});
}

// When the paths diverge but not in the first component, it's possible for the
// $addFields to affect the $match result: it replaces some scalars with objects,
// which makes the $match path visit some missing fields instead of skipping them,
// which means an {$eq: null} predicate changes from false to true.
testDependent({
    stage1: {$addFields: {'a.x': 5}},
    stage2: {$match: {'a.y': null}},
    inputDoc: {a: [0]},
});
// Similarly {$in: [... null ...]} is a null-accepting predicate.
testDependent({
    stage1: {$addFields: {'a.x': 5}},
    stage2: {$match: {'a.y': {$in: [2, null]}}},
    inputDoc: {a: [0]},
});
// Many predicates do not match null, and those are not affected by this edge case.
{
    const stage1 = {$addFields: {'a.x': 5}};
    const stage2 = {$match: {'a.y': 7}};
    testIndependent({stage1, stage2, inputDoc: {a: {y: 6}}});
    testIndependent({stage1, stage2, inputDoc: {a: {y: 7}}});
    testIndependent({stage1, stage2, inputDoc: {a: [{y: 6}]}});
    testIndependent({stage1, stage2, inputDoc: {a: [{y: 7}]}});
    testIndependent({stage1, stage2, inputDoc: {a: [0, {y: 6}]}});
    testIndependent({stage1, stage2, inputDoc: {a: [0, {y: 7}]}});
}

// Numeric paths make it harder to tell when two paths diverge.
// 'a.0' and 'a.x' look like they diverge in their second component,
// but because of implicit array traversal they can both touch $$ROOT["a"][0]["x"].
testDependent({
    stage1: {$addFields: {'a.x': 5}},
    stage2: {$match: {'a.0': 3}},
    inputDoc: {a: [3]},
});

// ExpressionFieldPath such as "$a.y" has a different behavior from MatchExpression paths:
// it preserves the shape of all the arrays it traverses through, and it skips over any missing
// fields or non-objects. This means when $addFields turns scalars into objects, it doesn't affect
// the result of the "$a.y" expression.
//
// Numeric paths are not tested here because ExpressionFieldPath does not treat numeric field paths
// specially: in "$a.0" the 0 is treated as an object field name.
// See 'ExpressionFieldPath::evaluatePath'.
{
    const stage1 = {$addFields: {'a.x': 5}};
    const stage2 = {$addFields: {result: "$a.y"}};
    // Non-array cases.
    testIndependent({stage1, stage2, inputDoc: {}});
    testIndependent({stage1, stage2, inputDoc: {a: 0}});
    testIndependent({stage1, stage2, inputDoc: {a: {}}});
    testIndependent({stage1, stage2, inputDoc: {a: {x: 0}}});
    testIndependent({stage1, stage2, inputDoc: {a: {x: 0, y: 7}}});
    // Dot encounters an array.
    testIndependent({stage1, stage2, inputDoc: {a: [0]}});
    testIndependent({stage1, stage2, inputDoc: {a: [{}]}});
    testIndependent({stage1, stage2, inputDoc: {a: [{x: 0}]}});
    testIndependent({stage1, stage2, inputDoc: {a: [{x: 0, y: 7}]}});
    // Multiple array elements.
    testIndependent({
        stage1,
        stage2,
        inputDoc: {
            a: [
                0,
                {},
                {x: 0},
                {x: 0, y: 7},
                [[{x: 0, y: 7}]],  // With different nesting.
            ]
        }
    });
}

// When a computed projection is simple enough, we can recognize that it's
// only renaming a field (not doing a complex modification).
testEquivalent({
    inputDoc: {a: 0, x: 5},
    pipeline1: [{$addFields: {a: "$x"}}, {$_internalInhibitOptimization: {}}, {$match: {a: 5}}],
    pipeline2: [{$match: {x: 5}}, {$addFields: {a: "$x"}}]
});

// However when either side is dotted then it's not a simple rename.
// a: "$x.y" is not a rename because the "$x.y" changes the array structure.
// It brings together two separate arrays creating a doubly-nested array: [[5]].
testNotEquivalent({
    inputDoc: {a: 0, x: [{y: [5]}]},
    correct: [{$addFields: {a: "$x.y"}}, {$match: {a: 5}}],
    incorrect: [{$match: {'x.y': 5}}, {$addFields: {a: "$x.y"}}],
});
// 'a.b': "$x" is not a rename for several reasons.
{
    const correct = [{$addFields: {'a.b': "$x"}}, {$match: {'a.b': 5}}];
    const incorrect = [{$match: {x: 5}}, {$addFields: {a: "$x.y"}}];
    // 'a.b' can descend into doubly nested arrays.
    testNotEquivalent({correct, incorrect, inputDoc: {a: [[0]], x: 5}});
    // 'a.b' can point to zero locations.
    testNotEquivalent({correct, incorrect, inputDoc: {a: [], x: 5}});
}
