/**
 * Helper functions for generating of queries over a collection.
 */
function makeMatchPredicate(field, boundary, compOp) {
    return {"$match": {[field]: {[compOp]: boundary}}};
}

function makeRangePredicate(field, op1, bound1, op2, bound2, isElemMatch = false) {
    if (isElemMatch) {
        return {"$match": {[field]: {"$elemMatch": {[op1]: bound1, [op2]: bound2}}}};
    }
    return {"$match": {[field]: {[op1]: bound1, [op2]: bound2}}};
}

/**
 * Generates predicates with comparison operators for the given field and boundary values.
 * We want to cover all comparisons and multiple values in the field domain. To avoid combinatoric
 * explosion in the number of predicates we create all comparison predicates only for 25% of the
 * query values, while for the other 75% we pick one comparison operator in a round-robin fashion.
 */
function generateComparisons(field, boundaries, fieldType) {
    let predicates = [];
    const compOps = ["$eq", "$lt", "$lte", "$gt", "$gte"];
    // Index over boundaries.
    let i = 0;
    // Index over comparison operators.
    let j = 0;
    let docs = [];
    boundaries.forEach(function(boundary) {
        if (i % 4 == 1) {
            for (const op of compOps) {
                const pred = makeMatchPredicate(field, boundary, op);
                const doc = {"pipeline": [pred], "qtype": op, "dtype": fieldType};
                docs.push(doc);
            }
        } else {
            const pred = makeMatchPredicate(field, boundary, compOps[j]);
            const doc = {"pipeline": [pred], "qtype": compOps[j], "dtype": fieldType};
            docs.push(doc);
            j = (j + 1) % 5;
        }
        i++;
    });
    return docs;
}

/**
 * Helper function to generate an array of query ranges given an array of boundary values. The
 * 'step' parameter determines the size of the ranges.
 */
function generateRanges(values, step) {
    let ranges = [];
    let i = 0;
    while (i + step < values.length) {
        ranges.push([values[i], values[i + step]]);
        i = (step > 1) ? i + 1 : i + 2;
    }
    return ranges;
}

/**
 * Split an ordered array of values into sub-arrays of the same type.
 * Example: [0, 25, 'an', 'mac', 'zen'] -> [[0, 25], ['an', 'mac', 'zen']].
 */
function splitValuesPerType(values) {
    let tp = typeof values[0];
    let changePos = [0];
    let i = 1;
    while (i < values.length) {
        if (tp != typeof values[i]) {
            changePos.push(i);
            tp = typeof values[i];
        }
        i++;
    }
    changePos.push(values.length);
    let typedValues = [];
    let j = 0;
    while (j + 1 < changePos.length) {
        typedValues.push(values.slice(changePos[j], changePos[j + 1]));
        j++;
    }
    return typedValues;
}

/**
 * Generate range predicates with $match and $elemMatch over the 'field' using the values in the
 * array 'values'. isSmall is a boolean flag to generate small ranges.
 */
function generateRangePredicates(field, values, isSmall, fieldType) {
    const qtype = isSmall ? "small range" : "large range";
    const step = isSmall ? 1 : 3;
    const op1Option = ["$gt", "$gte"];
    const op2Option = ["$lt", "$lte"];

    let ranges = [];
    if (fieldType == 'mixed') {
        const typedValues = splitValuesPerType(values);
        for (const tv of typedValues) {
            const subRanges = generateRanges(tv, step);
            ranges = ranges.concat(subRanges);
        }
    } else {
        ranges = generateRanges(values, step);
    }

    let docs = [];

    ranges.forEach(function(range) {
        assert(range.length == 2);
        for (let op1 of op1Option) {
            for (let op2 of op2Option) {
                pred = makeRangePredicate(field, op1, range[0], op2, range[1]);
                const doc = {"pipeline": [pred], "qtype": qtype, "dtype": fieldType};
                docs.push(doc);
                if (fieldType == 'array' && range[0] <= range[1]) {
                    pred = makeRangePredicate(field, op1, range[0], op2, range[1], true);
                    const doc =
                        {"pipeline": [pred], "qtype": qtype, "dtype": fieldType, "elemMatch": true};
                    docs.push(doc);
                }
            }
        }
    });

    return docs;
}

/**
 * Helper function to extract positions for a sample of size n from a collection.
 */
function selectSamplePos(collSize, n) {
    let samplePos = [];
    let step = Math.round(collSize / n);
    let offset = n * step - collSize;

    let pos = (offset >= 0) ? Math.min(Math.trunc(step / 2), step - offset)
                            : Math.trunc(step / 2 + Math.abs(offset) / 2);
    while (pos < collSize) {
        samplePos.push(pos);
        pos += step;
    }
    return samplePos;
}

function selectSample(coll, samplePos) {
    return coll.aggregate([{$match: {"_id": {$in: samplePos}}}]).toArray();
}

function selectFieldValues(sample, field) {
    let values = [];
    for (const doc of sample) {
        values.push(doc[field]);
    }
    return values;
}

/**
 * Selects few values from histogram bucket boundaries.
 */
function selectHistogramBounds(statsColl, field, fieldType) {
    let values = [];
    let stats = statsColl.find({"_id": field})[0];
    // Specify which bucket bound to choose from each histogram type. The number is ratio of the
    // histogram size.
    let histSpec = {"minHistogram": 0.1, "maxHistogram": 0.9, "uniqueHistogram": 0.5};
    if (fieldType === 'array') {
        for (const key in histSpec) {
            const bounds = stats.statistics.arrayStatistics[key].bounds;
            const len = bounds.length;
            if (len > 2) {
                const i = Math.trunc(histSpec[key] * len);
                values.push(bounds[i]);
            }
        }
    } else {
        const bounds = stats.statistics.scalarHistogram.bounds;
        const len = bounds.length;
        if (len > 2) {
            for (const key in histSpec) {
                // Use the same positions from the scalar histogram.
                const i = Math.trunc(histSpec[key] * len);
                values.push(bounds[i]);
            }
        }
    }
    return values;
}

// Extract min/max values from a field. The initial unwind phase extracts the values in case the
// field contains arrays.
function getMinMax(coll, field) {
    const res = coll.aggregate([
                        {$unwind: field},
                        {$group: {_id: null, min: {$min: field}, max: {$max: field}}},
                        {$project: {_id: 0}}
                    ])
                    .toArray();
    return res[0];
}

function sortValues(values) {
    let sortColl = db["sortColl"];
    sortColl.drop();
    for (const x of values) {
        sortColl.insertOne({"a": x});
    }
    let res = sortColl.aggregate([{$sort: {a: 1}}, {$project: {_id: 0}}]).toArray();
    let sorted = [];
    for (const doc of res) {
        sorted.push(doc["a"]);
    }
    return sorted;
}

function deduplicate(boundaries) {
    let values = [boundaries[0]];
    let i = 0;
    while (i + 1 < boundaries.length) {
        if (boundaries[i] != boundaries[i + 1]) {
            values.push(boundaries[i + 1]);
        }
        i++;
    }
    return values;
}

/**
 * Select values from the collection to be used in the queries against it.
 * For each field we select values from several sources:
 * 1. Minimal and maximal values in the field;
 * 2. Values from a collection sample. The sample is specified as an array of document ids in the
 * 'samplePos' argument.
 * 3. Values from histogram bucket boundaries;
 * 4. TODO: Out-of-range values.
 * The function returns a document with keys- field names and values - arrays of selected query
 * values for the respective field.
 */
function selectQueryValues(coll, fields, fieldTypes, samplePos, statsColl) {
    const sample = selectSample(coll, samplePos);

    let queryValues = {};
    let i = 0;
    while (i < fields.length) {
        const field = fields[i];
        const fieldType = fieldTypes[i];
        const minMaxDoc = getMinMax(coll, "$" + field);
        // TODO: using min/ max values and the field type, add out-of-range values.

        let v = selectFieldValues(sample, field);
        if (fieldType === 'array') {
            v = v.flat();
        }

        v.push(minMaxDoc["min"]);
        v.push(minMaxDoc["max"]);

        let histValues = selectHistogramBounds(statsColl, field, fieldType);
        v = v.concat(histValues);

        let values = sortValues(v);

        queryValues[[field]] = deduplicate(values);
        i++;
    }
    jsTestLog(`Selected query values: ${tojsononeline(queryValues)}`);

    return queryValues;
}

/**
 * Query generation for a collection 'coll' with given fields and field types.
 * The generation uses values from a collection sample with 'sampleSize'.
 */
function generateQueries(coll, fields, fieldTypes, collSize, sampleSize, statsColl) {
    const samplePos = selectSamplePos(collSize, sampleSize);
    jsTestLog(`Sample positions: ${tojsononeline(samplePos)}\n`);

    const queryValues = selectQueryValues(coll, fields, fieldTypes, samplePos, statsColl);

    let testCases = [];
    let i = 0;
    while (i < fields.length) {
        const field = fields[i];
        const fieldType = fieldTypes[i];
        testCases = testCases.concat(generateComparisons(field, queryValues[field], fieldType));

        testCases = testCases.concat(
            generateRangePredicates(field, queryValues[field], true /* small range */, fieldType));
        testCases =
            testCases.concat(generateRangePredicates(field, queryValues[field], false, fieldType));
        i++;
    }

    i = 0;
    for (let query of testCases) {
        query["_id"] = i++;
    }

    return testCases;
}
