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
                const doc = {
                    "pipeline": [pred],
                    "qtype": op,
                    "dtype": fieldType,
                    "fieldName": field,
                    "elemMatch": false
                };
                docs.push(doc);
            }
        } else {
            const pred = makeMatchPredicate(field, boundary, compOps[j]);
            const doc = {
                "pipeline": [pred],
                "qtype": compOps[j],
                "dtype": fieldType,
                "fieldName": field,
                "elemMatch": false
            };
            docs.push(doc);
            j = (j + 1) % 5;
        }
        i++;
    });
    return docs;
}

const min_char_code = '0'.codePointAt(0);
const max_char_code = '~'.codePointAt(0);

function nextChar(thisChar, distance) {
    const number_of_chars = max_char_code - min_char_code + 1;
    const char_code = thisChar.codePointAt(0);
    assert(min_char_code <= char_code <= max_char_code, "char is out of range");
    const new_char_code =
        ((char_code - min_char_code + distance) % number_of_chars) + min_char_code;
    assert(min_char_code <= new_char_code <= max_char_code, "new char is out of range");
    return String.fromCodePoint(new_char_code);
}

/**
 * Produces a string value at some distance from the argument string.
 * distance: "small", "middle", "large".
 */
function nextStr(str, distance) {
    var res = 'nextStrUndefined';
    const spec = {"small": 3, "medium": 2, "large": 1};
    if (str.length == 0) {
        const nextCharCode = min_char_code + 4 - spec[distance];
        res = String.fromCodePoint(nextCharCode);
        assert(res.indexOf("NaN") == -1,
               `Found NaN with inputs: str=${str}, res=${res}, distance=${
                   distance}; min_char_code=${min_char_code}`);
    } else {
        let pos = spec[distance] - 1;
        if (pos >= str.length) {
            pos = str.length - 1;
        }

        let newStr0 = str.slice(0, pos);
        let nextCh = nextChar(str[pos], 4 - spec[distance] /*char distance*/);
        newStr1 = newStr0 + nextCh;
        newStr = newStr1 + str.slice(pos + 1, str.length);
        assert(newStr.indexOf("NaN") == -1,
               `Found NaN with inputs: newStr=${newStr}, str=${str}, distance=${distance}; pos=${
                   pos}, nextCh=${nextCh}, newStr0=${newStr0}, newStr1=${newStr1}`);
        res = newStr;
    }
    assert(res !== undefined,
           `Found undefined with inputs: str=${str}, res=${res}, distance=${distance}`);
    return res;
}

/**
 * Helper function to generate an array of query ranges given an array of boundary values. The
 * 'rangeSize' parameter determines the size of the ranges and varies depending on the 'fieldType'.
 * For fieldType integer 'rangeSize' is the amount to add to the low bound to compute the upper
 * bound. For fieldType string 'rangeSize' is one of "small", "medium", "large". For other data
 * types both low and upper bounds are taken from the 'values' array and rangeSize is the distance
 * they are apart from each other.
 */
function generateRanges(values, fieldType, rangeSize) {
    let ranges = [];
    if (fieldType == 'integer' || fieldType == 'double') {
        for (const val of values) {
            ranges.push([val, val + rangeSize]);
        }
    } else if (fieldType == 'string') {
        for (const val of values) {
            nanPos = val.indexOf("NaN");
            assert(nanPos == -1, `Found NaN in values: ${values}, ${val}, ${nanPos}`);
            var nextVar = nextStr(val, rangeSize);
            assert(nextVar != 'nextStrUndefined',
                   `Found nextStrUndefined in values: ${values}, nextVar: ${nextVar}, val=${
                       val}, rangeSize=${rangeSize}`);
            assert(nextVar !== undefined,
                   `Found undefined in values: ${values}, nextVar: ${nextVar}, val=${
                       val}, rangeSize=${rangeSize}`);
            ranges.push([val, nextVar]);
        }
    } else if (fieldType == 'date') {
        for (const val of values) {
            const minDate = new Date(val);
            ranges.push([minDate, new Date(minDate.getTime() + rangeSize)]);
        }
    } else {
        // For other data types use the values array to form the ranges.
        let i = 0;
        const step = rangeSize;
        while (i + step < values.length) {
            let highBound = values[i + step];
            assert(highBound !== undefined,
                   `Undefined value, inputs: (values=${values}, fieldType=${fieldType}, rangeSize=${
                       rangeSize})`);
            ranges.push([values[i], highBound]);
            i++;
        }
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

function getTypeFromFieldName(fieldName) {
    const fieldMeta = fieldName.split("_");
    let elemType = undefined;
    for (fieldPart of fieldMeta) {
        if (fieldPart == "int") {
            elemType = "integer";
        } else if (fieldPart == "dbl") {
            elemType = "double";
        } else if (fieldPart == "str") {
            elemType = "string";
        } else if (fieldPart == "dt") {
            elemType = "date";
        }
        if (elemType !== undefined) {
            return elemType;
        }
    }
    assert(false, `getTypeFromFieldName didn't find a field type in ${fieldName}`);
    return elemType;
}

/**
 * Generate range predicates with $match and $elemMatch over the 'field' using the values specified
 * in the 'queryValues' document: {values: [1, 15, 37, 72, 100], min: 1, max: 100}. The 'values'
 * array is sorted.
 */
function generateRangePredicates(field, queryValues, fieldType) {
    const querySpecs = {"small": 0.001, "medium": 0.01, "large": 0.1};

    const opOptions = [["$gt", "$lt"], ["$gt", "$lte"], ["$gte", "$lt"], ["$gte", "$lte"]];
    // Index over comparison operators to choose them in a round robin fashion.
    let j = 0;

    let elemType = fieldType;
    if (fieldType == 'array') {
        elemType = getTypeFromFieldName(field);
    }
    let docs = [];
    for (const qSize in querySpecs) {
        let ranges = [];
        if (elemType == 'integer' || elemType == 'double') {
            var valueDiff = (queryValues["max"] - queryValues["min"]);
            const rangeSize = Math.round(valueDiff * querySpecs[qSize]);
            if (rangeSize < 2 && elemType == 'integer') {
                continue;
            }
            ranges = generateRanges(queryValues["values"], elemType, rangeSize);
        } else if (elemType == 'string') {
            ranges = generateRanges(queryValues["values"], elemType, qSize);
        } else if (elemType == 'date') {
            const minDate = new Date(queryValues["min"]);
            const maxDate = new Date(queryValues["max"]);
            const rangeSize =
                Math.round((maxDate.getTime() - minDate.getTime()) * querySpecs[qSize]);
            ranges = generateRanges(queryValues["values"], elemType, rangeSize);
        } else {
            const step = (qSize == "small") ? 1 : (qSize == "medium") ? 2 : 3;
            ranges = generateRanges(queryValues["values"], elemType, step);
        }

        ranges.forEach(function(range) {
            assert(range.length == 2);
            let [op1, op2] = opOptions[j];
            pred = makeRangePredicate(field, op1, range[0], op2, range[1]);
            const doc = {
                "pipeline": [pred],
                "qtype": qSize + " range",
                "dtype": fieldType,
                "fieldName": field,
                "elemMatch": false
            };
            docs.push(doc);
            if (fieldType == 'array' && range[0] <= range[1]) {
                pred = makeRangePredicate(field, op1, range[0], op2, range[1], true);
                const doc = {
                    "pipeline": [pred],
                    "qtype": qSize + " range",
                    "dtype": fieldType,
                    "fieldName": field,
                    "elemMatch": true
                };
                docs.push(doc);
            }
            j = (j + 1) % opOptions.length;
        });
    }

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
    jsTestLog(`Sample positions: ${tojsononeline(samplePos)}\n`);
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

/**
 * Extract min/max values from a field. The initial unwind phase extracts the values in case the
 * field contains arrays.
 */
function getMinMax(coll, field) {
    const res = coll.aggregate([
                        {$unwind: field},
                        {$group: {_id: null, min: {$min: field}, max: {$max: field}}},
                        {$project: {_id: 0}}
                    ])
                    .toArray();
    return res[0];
}

/**
 * Extract query values from an array of sample arrays. Select up to three values per array element.
 * {[1, 3, 5], [ 2, 4, 6, 8, 10], [100]] -> [1, 3, 5, 2, 6, 10, 100]
 */
function selectArrayValues(nestedArray) {
    let values = [];
    nestedArray.forEach(function(array) {
        if (typeof array != "object") {
            values.push(array);
        } else {
            const len = array.length;
            if (len <= 3) {
                values = values.concat(array);
            } else {
                for (let ratio of [0.1, 0.5, 0.9]) {
                    const i = Math.trunc(ratio * len);
                    values.push(array[i]);
                }
            }
        }
    });
    return values;
}

function selectOutOfRangeValues(minMaxDoc, fieldType) {
    let values = [];
    const validTypes = new Set(["integer", "double", "string", "date"]);
    if (!validTypes.has(fieldType)) {
        return values;
    }

    const oneHour = 60 * 60 * 1000;
    const min = minMaxDoc["min"];
    if (fieldType == 'integer' || fieldType == 'double') {
        values.push(min - 1);
    } else if (fieldType == 'string') {
        const prevMin = (min.length > 1) ? min.at(0) : "";
        values.push(prevMin);
    } else if (fieldType == 'date') {
        const minDate = new Date(min);
        values.push(new Date(minDate.getTime() - oneHour));
    }

    const max = minMaxDoc["max"];
    if (fieldType == 'integer' || fieldType == 'double') {
        values.push(max + 1);
    } else if (fieldType == 'string') {
        values.push(nextStr(max, "small"));
    } else if (fieldType == 'date') {
        const maxDate = new Date(max);
        values.push(new Date(maxDate.getTime() + oneHour));
    }
    return values;
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
 * The function returns a document with keys- field names and values - documents of selected query
 * values, min, and max for the respective field. Example:
 * {"a": {values: [1, 15, 37, 72, 100], min: 1, max: 100}, "b": {...} }
 */
function selectQueryValues(coll, fields, fieldTypes, samplePos, statsColl) {
    const sample = selectSample(coll, samplePos);

    let queryValues = {};
    let i = 0;
    while (i < fields.length) {
        const field = fields[i];
        const fieldType = fieldTypes[i];

        let v = selectFieldValues(sample, field);
        if (fieldType === 'array') {
            v = selectArrayValues(v);
        }

        const minMaxDoc = getMinMax(coll, "$" + field);
        // v.push(minMaxDoc["min"]);
        // v.push(minMaxDoc["max"]);

        // Using min/ max values extract out-of-range values.
        // const outOfRange = selectOutOfRangeValues(minMaxDoc, fieldType);
        // v = v.concat(outOfRange);

        const histValues = selectHistogramBounds(statsColl, field, fieldType);
        v = v.concat(histValues);

        let values = sortValues(v);

        queryValues[[field]] = {
            values: deduplicate(values),
            min: minMaxDoc["min"],
            max: minMaxDoc["max"]
        };
        i++;
    }
    jsTestLog(`Selected query values: ${tojson(queryValues)}`);

    return queryValues;
}

/**
 * Query generation for a collection 'coll' with given fields and field types.
 * The generation uses values from a collection sample with 'sampleSize'.
 */
function generateQueries(fields, fieldTypes, queryValues) {
    let testCases = [];
    let i = 0;
    while (i < fields.length) {
        const field = fields[i];
        const fieldType = fieldTypes[i];
        testCases =
            testCases.concat(generateComparisons(field, queryValues[field].values, fieldType));

        testCases = testCases.concat(generateRangePredicates(field, queryValues[field], fieldType));
        i++;
    }

    i = 0;
    for (let query of testCases) {
        query["_id"] = i++;
    }

    return testCases;
}

/**
 * Generates multi-field conjunctions by selecting terms from the single-field predicates in
 * 'testCases'. Uses recursion on the position for which we pick a term predicate.
 * - cnt: number of terms in the predicate curPos: the position of the current predicate
 * - chosenIds: array of chosen predicate ids refering to the testCases array
 * - chosenFields: set of field names chosen so far, to avoid predicates over the same field
 * - step: step to navigate through the testCases array
 * - predicates: array of result predicate documents
 */
function pickNextTerm(testCases, cnt, curPos, chosenIds, chosenFields, step, predicates) {
    assert.eq(curPos, chosenIds.length);
    let i = (curPos == 0) ? 0 : chosenIds.at(-1) + 1;

    while (i < testCases.length) {
        if (testCases[i].nReturned == 0 || chosenFields.has(testCases[i].fieldName)) {
            i++;
            continue;
        }
        chosenIds.push(i);
        chosenFields.add(testCases[i].fieldName);
        if (curPos == cnt - 1) {
            // We have picked the last term, create the composite predicate.
            let terms = [];
            for (const id of chosenIds) {
                terms.push(testCases[id].pipeline[0]["$match"]);
            }
            const fields = Array.from(chosenFields);
            let doc = {
                "pipeline": [{$match: {$and: terms}}],
                "qtype": "conjunction",
                "numberOfTerms": cnt,
                "fieldNames": fields
            };
            predicates.push(doc);
        } else {
            // Pick next term.
            pickNextTerm(testCases, cnt, curPos + 1, chosenIds, chosenFields, step, predicates);
        }
        chosenIds.pop();
        chosenFields.delete(testCases[i].fieldName);
        i = i + step;
    }
}

/**
 * Make a conjunction or disjunction over a single field.
 * op: $and or $or
 * comp: array of comparisons for predicate terms
 */
function makeSingleFieldComplexPredicate(field, values, op, comp, predicates, isArray = false) {
    let terms = [];
    for (let i = 0; i < comp.length; i++) {
        terms.push({[field]: {[comp[i]]: values[i]}});
    }
    let qtype = (op == "$or") ? "disj1field" : "conj1field";
    if (isArray) {
        qtype = qtype + "array";
    }

    let doc = {
        "pipeline": [{$match: {[op]: terms}}],
        "qtype": qtype,
        "numberOfTerms": comp.length,
        "fieldNames": [field]
    };
    predicates.push(doc);
}

/**
 * Make a single field DNF predicate.
 */
function makeSingleFieldDNF(field, values, predicates) {
    let term1 = {"$and": [{[field]: {"$gt": values[0]}}, {[field]: {"$lt": values[1]}}]};
    let term2 = {"$and": [{[field]: {"$gte": values[2]}}, {[field]: {"$lt": values[3]}}]};

    let doc = {
        "pipeline": [{$match: {"$or": [term1, term2]}}],
        "qtype": "DNF1field",
        "numberOfTerms": 4,
        "fieldNames": [field]
    };
    predicates.push(doc);
}

/**
 * Generate single-field conjunctions and disjunctions using values from the 'queryValues' document.
 */
function generateSingleFieldPredicates(fields, fieldTypes, queryValues, predicates) {
    let i = 0;
    while (i < fields.length) {
        const field = fields[i];
        const fieldType = fieldTypes[i];
        let values = queryValues[field].values;
        // Remove outlier values.
        if (values.length >= 8) {
            values = values.slice(2, values.length - 2);
        }

        while (values.length > 4) {
            makeSingleFieldComplexPredicate(field, values, "$and", ["$gte", "$lte"], predicates);
            makeSingleFieldComplexPredicate(
                field, values, "$and", ["$gt", "$gte", "$lt", "$lte"], predicates);
            if (fieldType == 'array') {
                // Make non-overlapping conjunctions for multi-key fields.
                makeSingleFieldComplexPredicate(
                    field, values, "$and", ["$lte", "$gte"], predicates, true);
                makeSingleFieldComplexPredicate(
                    field, values, "$and", ["$lte", "$eq"], predicates, true);
                makeSingleFieldComplexPredicate(
                    field, values, "$and", ["$gt", "$lte", "$gte", "$lt"], predicates, true);
            }
            makeSingleFieldComplexPredicate(field, values, "$or", ["$lte", "$gte"], predicates);
            makeSingleFieldComplexPredicate(field, values, "$or", ["$eq", "$gte"], predicates);
            makeSingleFieldComplexPredicate(
                field, values, "$or", ["$lt", "$eq", "$eq", "$gte"], predicates);
            makeSingleFieldDNF(field, values, predicates);
            values = values.slice(4, values.length);
        }
        i++;
    }
}

/**
 * Generate complex predicates:
 * - multi-field conjunctions with 2, 4, and 7 terms by combining single-field simple predicates
 * from the 'testCases' array.
 * - single-field conjunctions and disjunctions with 2 and 4 terms.
 * - single-field DNFs.
 */
function generateComplexPredicates(testCases, fields, fieldTypes, queryValues) {
    let predicates = [];
    // Generate multi-field conjunctions.
    let chosenFields = new Set();
    let chosenIds = [];
    const targetCount = testCases.length / 6;
    for (let termCount of [2, 4, 7]) {
        const countPerTerm = Math.pow(targetCount, 1 / termCount);
        const step = Math.trunc(testCases.length / countPerTerm);
        pickNextTerm(testCases, termCount, 0, chosenIds, chosenFields, step, predicates);
    }

    // Generate single-field disjunctions and conjunctions.
    generateSingleFieldPredicates(fields, fieldTypes, queryValues, predicates);

    i = 0;
    for (let query of predicates) {
        query["_id"] = i++;
    }
    return predicates;
}
