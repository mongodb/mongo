/**
 * Functions used for various percentile and median tests
 * @tags: [
 *   requires_fcv_70,
 * ]
 */

/**
 * Functions for percentile and median as accumulators.
 */

// Tests for correctness without grouping. Each group gets its own accumulator so we can
// validate the basic $percentile functionality using a single group.
export function testWithSingleGroup({coll, docs, percentileSpec, letSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);
    const res =
        coll.aggregate([{$group: {_id: null, p: percentileSpec}}], {let : letSpec}).toArray();

    // For $percentile the result should be ordered to match the spec, so assert exact equality.
    assert.eq(expectedResult, res[0].p, msg + `; Result: ${tojson(res)}`);
}

export function testWithSingleGroupMedian({coll, docs, medianSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);

    let medianArgs = medianSpec["$median"];
    const percentileSpec = {
        $percentile: {input: medianArgs.input, method: medianArgs.method, p: [0.5]}
    };

    const medianRes = coll.aggregate([{$group: {_id: null, p: medianSpec}}]).toArray();
    const percentileRes = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    assert.eq(expectedResult, medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);

    // If all the data is non-numeric then the expected result is just null, and therefore
    // cannot be indexed into.
    assert.eq(percentileRes[0].p[0], medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);
}

export function testWithMultipleGroups({coll, docs, percentileSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);
    const res =
        coll.aggregate([{$group: {_id: "$k", p: percentileSpec}}, {$sort: {_id: 1}}]).toArray();

    // For $percentile the result should be ordered to match the spec, so assert exact equality.
    for (let i = 0; i < res.length; i++) {
        assert.eq(expectedResult[i], res[i].p, msg + ` result: ${tojson(res)}`);
    }
}

export function testWithMultipleGroupsMedian({coll, docs, medianSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);

    let medianArgs = medianSpec["$median"];
    const percentileSpec = {
        $percentile: {input: medianArgs.input, method: medianArgs.method, p: [0.5]}
    };

    const medianRes = coll.aggregate([{$group: {_id: null, p: medianSpec}}]).toArray();
    const percentileRes = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    assert.eq(medianRes.length, percentileRes.length);
    for (let i = 0; i < medianRes.length; i++) {
        assert.eq(expectedResult[i], medianRes[i].p, msg + ` result: ${tojson(medianRes)}`);
        assert.eq(percentileRes[i].p[0], medianRes[i].p, msg + ` result: ${tojson(medianRes)}`);
    }
}

// findRank returns index of the first value in sorted greater than or equal to the algorithm's
// computed percentile. Be cautious with repeat values.
function findRank(sorted, value) {
    for (let i = 0; i < sorted.length; i++) {
        if (sorted[i] >= value) {
            return i;
        }
    }
    return -1;
}

// 'sorted' is expected to contain a sorted array of all _numeric_ samples that are used to
// compute the percentile(s). 'error' should be specified as a percentage point.
function testWithAccuracyError({coll, docs, percentileSpec, sorted, error, msg}) {
    coll.drop();
    coll.insertMany(docs);
    const res = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    for (let i = 0; i < res[0].p.length; i++) {
        const p = percentileSpec.$percentile.p[i];
        if (percentileSpec.$percentile.method != "continuous") {
            const computedRank = findRank(sorted, res[0].p[i]);
            const trueRank = Math.max(0, Math.ceil(p * sorted.length) - 1);
            if (res[0].p[i] != sorted[trueRank]) {
                assert.lte(Math.abs(trueRank - computedRank),
                           error * sorted.length,
                           msg +
                               `; Error is too high for p: ${p}. Computed: ${
                                   res[0].p[i]} with rank ${computedRank} but the true value ${
                                   sorted[trueRank]} has rank ${trueRank} in ${tojson(res)}`);
            }
        } else {
            const computedRank = findRank(sorted, res[0].p[i]);
            const trueRank = p * (sorted.length - 1);
            const cr = Math.ceil(trueRank);
            const fr = Math.floor(trueRank);
            if (cr == trueRank && trueRank == fr) {
                assert.eq(res[0].p[i],
                          sorted[trueRank],
                          msg +
                              `; Computed: ${res[0].p[i]} but the true value is ${
                                  sorted[trueRank]} in ${tojson(res)}`);
            } else {
                const truePercentile = (cr - trueRank) * sorted[fr] + (trueRank - fr) * sorted[cr];
                assert.close(res[0].p[i],
                             truePercentile,
                             msg +
                                 `; Computed: ${res[0].p[i]} but the true value is ${
                                     truePercentile} in ${tojson(res)}`);
            }
        }
    }
}

export function testLargeUniformDataset(coll, samples, sortedSamples, p, accuracyError, method) {
    let docs = [];
    for (let i = 0; i < samples.length; i++) {
        docs.push({_id: i, x: samples[i]});
    }
    testWithAccuracyError({
        coll: coll,
        docs: docs,
        percentileSpec: {$percentile: {p: p, input: "$x", method: method}},
        sorted: sortedSamples,
        msg: "Single group of uniformly distributed data",
        error: accuracyError
    });
}

export function testLargeUniformDataset_WithInfinities(
    coll, samples, sortedSamples, p, accuracyError, method) {
    let docs = [];
    for (let i = 0; i < samples.length; i++) {
        docs.push({_id: i * 10, x: samples[i]});
        if (i % 2000 == 0) {
            docs.push({_id: i * 10 + 1, x: Infinity});
            docs.push({_id: i * 10 + 2, x: -Infinity});
        }
    }

    let sortedSamplesWithInfinities =
        Array(5).fill(-Infinity).concat(sortedSamples).concat(Array(5).fill(Infinity));

    testWithAccuracyError({
        coll: coll,
        docs: docs,
        percentileSpec: {$percentile: {p: p, input: "$x", method: method}},
        sorted: sortedSamplesWithInfinities,
        msg: "Single group of uniformly distributed data with infinite values",
        error: accuracyError
    });
}

export function testLargeUniformDataset_Decimal(
    coll, samples, sortedSamples, p, accuracyError, method) {
    let docs = [];
    for (let i = 0; i < samples.length; i++) {
        docs.push({_id: i * 10, x: NumberDecimal(samples[i])});
        if (i % 1000 == 0) {
            docs.push({_id: i * 10 + 1, x: NumberDecimal(NaN)});
            docs.push({_id: i * 10 + 2, x: NumberDecimal(-NaN)});
        }
    }
    testWithAccuracyError({
        coll: coll,
        docs: docs,
        percentileSpec: {$percentile: {p: p, input: "$x", method: method}},
        sorted: sortedSamples,
        msg: "Single group of uniformly distributed Decimal128 data",
        error: accuracyError
    });
}

/**
 * Functions for percentile and median as expressions.
 */
export function testWithProject({coll, doc, percentileSpec, letSpec, expectedResult, msg}) {
    coll.drop();
    coll.insert(doc);
    const res = coll.aggregate([{$project: {p: percentileSpec}}], {let : letSpec}).toArray();
    // For $percentile the result should be ordered to match the spec, so assert exact equality.
    assert.eq(expectedResult, res[0].p, msg + ` result: ${tojson(res)}`);
}

export function testWithProjectMedian({coll, doc, medianSpec, expectedResult, msg}) {
    coll.drop();
    coll.insert(doc);

    let medianArgs = medianSpec["$median"];
    const percentileSpec = {
        $percentile: {input: medianArgs.input, method: medianArgs.method, p: [0.5]}
    };

    const medianRes = coll.aggregate([{$project: {p: medianSpec}}]).toArray();
    const percentileRes = coll.aggregate([{$project: {p: percentileSpec}}]).toArray();

    assert.eq(expectedResult, medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);

    // If all the data is non-numeric then the expected result is just null, and therefore cannot be
    // indexed into.
    assert.eq(percentileRes[0].p[0], medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);
}

// 'rand()' generates a uniform distribution from [0.0, 1.0] so we can check accuracy of the
// result in terms of values rather than in terms of rank.
export function testLargeInput(coll, method) {
    Random.setRandomSeed(20230406);

    const n = 100000;
    let samples = [];
    for (let i = 0; i < n; i++) {
        samples.push(Random.rand());
    }
    let sortedSamples = [].concat(samples);
    sortedSamples.sort((a, b) => a - b);

    coll.drop();
    coll.insert({x: samples});
    const ps = [0.5, 0.999, 0.0001];
    const res =
        coll.aggregate([{$project: {p: {$percentile: {p: ps, input: "$x", method: method}}}}])
            .toArray();

    for (let i = 0; i < ps.length; i++) {
        let pctl = res[0].p[i];
        assert.lt(ps[i] - 0.01, pctl, `p = ${ps[i]} left bound`);
        assert.lt(pctl, ps[i] + 0.01, `p = ${ps[i]} right bound`);
    }
}

export function testLargeNonNumericInput(coll, method) {
    const n = 100000;
    let samples = [];
    for (let i = 0; i < n; i++) {
        samples.push([i]);
    }

    testWithProject({
        coll: coll,
        doc: {x: samples},
        percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: method}},
        expectedResult: [null, null, null],
        msg: "Multiple percentiles on large non-numeric input"
    });
}
