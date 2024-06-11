/**
 * This file implements densification in JavaScript to compare with the output from the $densify
 * stage.
 */

export const densifyUnits = [null, "millisecond", "second", "day", "month", "quarter", "year"];
export const interestingSteps = [1, 2, 3, 4, 5, 7];

/**
 * The code is made a lot shorter by relying on accessing properties on Date objects with
 * the object lookup syntax.
 * @param {String} unitName
 * @param {Number} factor
 * @returns functions to immutably add/subtract a specific duration with a date.
 */
export const makeArithmeticHelpers = (unitName, factor) => {
    const getTimeInUnits = date => {
        const newDate = new ISODate(date.toISOString());
        // Calling the proper function on the passed in date object. If the unitName was "Seconds"
        // would be equivalent to `newDate.getSeconds()`.
        return newDate["getUTC" + unitName]();
    };

    // Return a new date with the proper unit adjusted with the second parameter.
    // Dates and the setter helpers are generally mutable, but this function will make sure
    // the arithmetic helpers won't mutate their inputs.
    const setTimeInUnits = (date, newComponent) => {
        const newDate = new ISODate(date.toISOString());
        newDate["setUTC" + unitName](newComponent);
        return newDate;
    };

    const add = (date, step) => setTimeInUnits(date, getTimeInUnits(date) + (step * factor));
    const sub = (date, step) => setTimeInUnits(date, getTimeInUnits(date) - (step * factor));

    return {add: add, sub: sub};
};

/**
 * This function specifies the functions for performing arithmetic on densify values. A
 * null/undefined unitName will return functions for numbers rather than dates.
 * @param {String | null} unitName
 */
export const getArithmeticFunctionsForUnit = (unitName) => {
    switch (unitName) {
        case "millisecond":
            return makeArithmeticHelpers("Milliseconds", 1);
        case "second":
            return makeArithmeticHelpers("Seconds", 1);
        case "minute":
            return makeArithmeticHelpers("Minutes", 1);
        case "hour":
            return makeArithmeticHelpers("Hours", 1);
        case "day":
            return makeArithmeticHelpers("Date", 1);
        case "week":
            return makeArithmeticHelpers("Date", 7);
        case "month":
            return makeArithmeticHelpers("Month", 1);
        case "quarter":
            return makeArithmeticHelpers("Month", 3);
        case "year":
            return makeArithmeticHelpers("FullYear", 1);
        case null:  // missing unit means that we're dealing with numbers rather than dates.
        case undefined:
            return {add: (val, step) => val + step, sub: (val, step) => val - step};
    }
};

export function densifyInJS(stage, docs) {
    const field = stage.field;
    const {step, bounds, unit} = stage.range;
    const stream = [];

    // $densify is translated into a $sort on `field` and then $internalDensify, so replicate that
    // behavior here by sorting the array of documents by the field.
    docs.sort((a, b) => {
        if (a[field] == null && b[field] == null) {
            return 0;
        } else if (a[field] == null) {  // null << any value.
            return -1;
        } else if (b[field] == null) {
            return 1;
        } else {
            return a[field] - b[field];
        }
    });
    const docsWithoutNulls = docs.filter(doc => doc[field] != null);

    const {add, sub} = getArithmeticFunctionsForUnit(unit);

    function generateDocuments(min, max, pred) {
        const docs = [];
        while (min < max) {
            if (!pred || pred(min)) {
                docs.push({[field]: min});
            }
            min = add(min, step);
        }
        return docs;
    }

    // Explicit ranges always generate on-step relative to the lower-bound of the range,
    // this function encapsulates the logic to do that for dates (requires a loop since steps aren't
    // always constant sized).
    const getNextStepFromBase = (val, base, step) => {
        let nextStep = base;
        while (nextStep <= val) {
            nextStep = add(nextStep, step);
        }
        return nextStep;
    };

    if (bounds === "full") {
        if (docs.length == 0) {
            return stream;
        }
        const minValue = docsWithoutNulls[0][field];
        const maxValue = docsWithoutNulls[docsWithoutNulls.length - 1][field];
        return densifyInJS({field: stage.field, range: {step, bounds: [minValue, maxValue], unit}},
                           docs);
    } else if (bounds === "partition") {
        throw new Error("Partitioning not supported by JS densify.");
    } else if (bounds.length == 2) {
        const [lower, upper] = bounds;
        let currentVal = docsWithoutNulls.length > 0
            ? Math.min(docsWithoutNulls[0], sub(lower, step))
            : sub(lower, step);
        for (let i = 0; i < docs.length; i++) {
            const nextVal = docs[i][field];
            if (nextVal === null || nextVal === undefined) {
                // If the next value in the stream is missing or null, let the doc pass through
                // without modifying anything else.
                stream.push(docs[i]);
                continue;
            }
            stream.push(...generateDocuments(getNextStepFromBase(currentVal, lower, step),
                                             nextVal,
                                             (val) => val >= lower && val < upper));
            stream.push(docs[i]);
            currentVal = nextVal;
        }
        const lastVal = docsWithoutNulls.length > 0
            ? docsWithoutNulls[docsWithoutNulls.length - 1][field]
            : sub(lower, step);
        if (lastVal < upper) {
            stream.push(...generateDocuments(getNextStepFromBase(currentVal, lower, step), upper));
        }
    }
    return stream;
}

export const genRange = (min, max) => {
    const result = [];
    for (let i = min; i < max; i++) {
        result.push(i);
    }
    return result;
};

export const insertDocumentsFromOffsets = ({base, offsets, addFunc, coll, field}) =>
    offsets.forEach(num => coll.insert({[field || "val"]: addFunc(base, num)}));

export const insertDocumentsOnPredicate = ({base, min, max, pred, addFunc, coll, field}) =>
    insertDocumentsFromOffsets(
        {base, offsets: genRange(min, max).filter(pred), addFunc, coll, field});

export const insertDocumentsOnStep = ({base, min, max, step, addFunc, coll, field}) =>
    insertDocumentsOnPredicate(
        {base, min, max, pred: i => ((i - min) % step) === 0, addFunc, coll, field});

export function buildErrorString(found, expected) {
    return "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(found);
}

// Assert that densification in JavaScript and the $densify stage output the same documents.
export function testDensifyStage(stage, coll, msg = "") {
    if (stage.range.unit === null) {
        delete stage.range.unit;
    }
    const result = coll.aggregate([{"$densify": stage}]).toArray();
    const expected = densifyInJS(stage, coll.find({}).toArray());
    const newMsg = msg + " | stage: " + tojson(stage);
    assert.sameMembers(expected, result, newMsg + buildErrorString(result, expected));
}
