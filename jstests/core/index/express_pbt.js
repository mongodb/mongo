/**
 * This is a property-based test for the Express execution path. It
 * tests that query results match when using the express path and when we
 * include a hint(), which disables the express path.
 *
 * It also verifies that _id-based update() and remove() operations
 * perform as expected.
 *
 * @tags: [
 * requires_fcv_80
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const fieldArb = fc.constantFrom('_id', 'a', 'b', 'c', 'd', 'e', 'f');
const projectFieldsArb = fc.uniqueArray(fieldArb, {minLength: 0, maxLength: 7});
const projectArb =
    fc.record({fields: projectFieldsArb, idIncluded: fc.boolean(), isInclusive: fc.boolean()})
        .map(function({fields, idIncluded, isInclusive}) {
            const projectList = {};
            for (const field of fields) {
                projectList[field] = field === '_id' ? idIncluded : isInclusive;
            }
            return projectList;
        });

const directionArb = fc.constantFrom(1, -1);
const indexSpecArb = fc.dictionary(fieldArb, directionArb, {minKeys: 1, maxKeys: 8});
const fieldValueArb = fc.oneof(
    fc.boolean(),
    fc.integer({min: 1, max: 10}),
    fc.constantFrom("foo", "bar", "baz"),
    fc.date({min: new Date('1991-01-01T00:00:00.000Z'), max: new Date('2001-01-01T00:00:00.000Z')}),
    fc.constant(null),
);

const arrayFieldValueArb = fc.oneof(
    fc.array(fc.boolean(), {maxLength: 3}),
    fc.array(fc.integer(), {maxLength: 3}),
    fc.array(fc.constantFrom("foo", "bar", "baz", ""), {maxLength: 4}),
    fc.array(
        fc.date(
            {min: new Date('1991-01-01T00:00:00.000Z'), max: new Date('2001-01-01T00:00:00.000Z')}),
        {maxLength: 4}),
);

const documentArb = fc.record({
    _id: fieldValueArb.filter(val => val !== null),  // _id cannot be null
    a: arrayFieldValueArb,
    b: fieldValueArb,
    c: fieldValueArb,
    d: fieldValueArb,
    e: fieldValueArb,
    f: fieldValueArb,
},
                              {
                                  noNullPrototype: false,
                              });

// Arbitrary for all documents in the collection.
const docsArb = fc.array(documentArb, {minLength: 1, maxLength: 5});
const updateValueArb = fc.oneof(
    fc.integer({min: 10, max: 30}),
    fc.constantFrom("bee", "biz", "dog1"),
    fc.date({min: new Date('2002-01-01T00:00:00.000Z'), max: new Date('2026-01-01T00:00:00.000Z')}),
);

const testCaseArb = fc.record({
    indexSpec: indexSpecArb,
    docs: docsArb,
    projectSpec: projectArb,
    isIndexUnique: fc.boolean(),
    isClustered: fc.boolean(),
    updateValue: updateValueArb,
});

function hasDuplicates(arr) {
    return new Set(arr).size !== arr.length;
}

function arrayContainsElement(arr, elem) {
    if (!Array.isArray(elem)) {
        elem = [elem];
    }
    if (_resultSetsEqualUnordered(arr, elem)) {
        return true;
    }
    for (const a of arr) {
        if (_resultSetsEqualUnordered([a], elem)) {
            return true;
        }
    }
    return false;
}

function verifyReadOperations(collection, query, projectSpec) {
    const expressRes = collection.find(query, projectSpec).toArray();
    const fallbackRes = collection.find(query, projectSpec).hint({_id: 1}).toArray();
    let agg = [{$match: query}];
    if (Object.keys(projectSpec).length > 0) {
        agg.push({$project: projectSpec});
    }
    const expressAggRes = collection.aggregate(agg).toArray();
    assert(_resultSetsEqualUnordered(expressRes, fallbackRes));
    assert(_resultSetsEqualUnordered(expressAggRes, fallbackRes));

    // adding limit(1) enables some additional queries to become Express
    // eligible, but since we can't have a sort(), we check against all possible
    // results.
    const expressLimitRes = collection.find(query, projectSpec).limit(1).toArray();
    agg.push({$limit: 1});
    const expressLimitAggRes = collection.aggregate(agg).toArray();
    const fallbackLimitRes = collection.find(query, projectSpec).hint({_id: 1}).toArray();

    assert(arrayContainsElement(fallbackLimitRes, expressLimitRes),
           [fallbackLimitRes, expressLimitRes]);
    assert(arrayContainsElement(fallbackLimitRes, expressLimitAggRes),
           [fallbackLimitRes, expressLimitAggRes]);
}

function verifyWriteOperations(collection, query, updateValue) {
    // Only test _id queries, since Express write operations don't support non-_id
    // indexes.
    if (query.hasOwnProperty("_id")) {
        let updateRes = collection.find(query).toArray()[0];
        updateRes.newField = updateValue;
        assert.commandWorked(collection.update(query, updateRes));
        updateRes = collection.find(query).toArray()[0];
        assert.eq(updateRes.newField, updateValue, updateRes);

        assert.commandWorked(collection.deleteOne(query));
        assert.eq(collection.find(query).toArray(), []);
    }
}

fc.assert(
    fc.property(testCaseArb,
                ({indexSpec, docs, projectSpec, isIndexUnique, isClustered, updateValue}) => {
                    const fields = Object.keys(indexSpec);
                    fc.pre(!hasDuplicates(docs.map(doc => doc._id.toString())));

                    fc.pre(!isIndexUnique ||
                           !hasDuplicates(docs.map(doc => {
                                                  let d = doc[fields[0]];
                                                  if (d == null || typeof d == "undefined") {
                                                      return "null";
                                                  } else if (Array.isArray(d) && d.length == 0) {
                                                      return "[]";
                                                  } else if (typeof d.getTime === 'function') {
                                                      return d.getTime();
                                                  } else {
                                                      return d;
                                                  }
                                              })
                                              .flat()));

                    const collName = jsTestName();
                    if (isClustered) {
                        assertDropAndRecreateCollection(
                            db, collName, {clusteredIndex: {key: {_id: 1}, unique: true}});
                    } else {
                        assertDropAndRecreateCollection(db, collName);
                    }
                    const coll = db[collName];

                    // don't create duplicative _id-only index
                    if ((!indexSpec.hasOwnProperty("_id") || fields.length != 1)) {
                        if (isIndexUnique && FixtureHelpers.isSharded(coll)) {
                            isIndexUnique = false;  // sharded collections can't have unique indexes
                        }
                        assert.commandWorked(coll.createIndex(indexSpec, {unique: isIndexUnique}));
                    }
                    assert.commandWorked(coll.insert(docs));

                    let queryList = [];
                    const indexes = ["_id", fields[0]];
                    indexes.forEach((curField) => {
                        const value = docs[0][curField];
                        queryList.push({[curField]: value});
                        if (Array.isArray(value) && value.length > 0) {
                            queryList.push({[curField]: value[0]});
                        }
                        // test queries with no expected matches too
                        queryList.push({[curField]: "no match"});
                        queryList.push({[curField]: 123});
                    });

                    jsTestLog(docs);
                    jsTestLog(indexSpec);
                    jsTestLog(projectSpec);
                    jsTestLog(queryList);

                    queryList.forEach((query) => {
                        verifyReadOperations(coll, query, projectSpec);
                    });

                    // queryList[0] should always produce exactly 1 match.
                    verifyWriteOperations(coll, queryList[0], updateValue);
                }),
    {
        seed: 413,
        // The search space for this PBT is small because express path covers a narrow range of
        // queries. 300 runs should be enough.
        numRuns: 300
    });
