/**
 * Test that $lookup queries work properly when there are dates outside the 32 bit epoch range in
 * the foreign collection. [1970-01-01 00:00:00 UTC - 2038-01-19 03:14:07 UTC].
 *
 * @tags: [
 *   requires_fcv_82,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

const standardDocs = [
    {_id: 0, t: ISODate("1975-12-01"), m: 0},
    {_id: 1, t: ISODate("1980-01-13"), m: 1},
    {_id: 2, t: ISODate("2018-07-14"), m: 2},
    {_id: 3, t: ISODate("2030-09-30"), m: 3},
];
const beforeEpochDocs = [
    {_id: 4, t: ISODate("1969-12-31T23:00:59.001Z"), m: 4},
    {_id: 5, t: ISODate("1969-12-31T23:59:59.001Z"), m: 5},  // 1ms before epoch
];
const afterEpochDocs = [
    {_id: 6, t: ISODate("2038-01-19T03:14:07.001Z"), m: 6},  // 1ms past 32-bit max secs since epoch
    {_id: 7, t: ISODate("2050-01-20T03:14:00.003Z"), m: 7}
];
const allExtendedRangeDocs = beforeEpochDocs.concat(afterEpochDocs);
const mixedDocs = standardDocs.concat(allExtendedRangeDocs);

function runTest(extendedRangeDocs) {
    const localColl = db.local_coll;
    const foreignColl = db.foreign_coll;
    assert(localColl.drop());
    assert(foreignColl.drop());

    // localColl is a regular collection, foreignColl is a time-series collection
    assert.commandWorked(localColl.insert([{a: 1}]));
    assert.commandWorked(db.createCollection(foreignColl.getName(), {
        timeseries: {timeField: "t", metaField: "m"},
    }));
    assert.commandWorked(foreignColl.createIndex({t: 1}));
    assert.commandWorked(foreignColl.insert(extendedRangeDocs));

    const pipeline =
        [{$lookup: {from: foreignColl.getName(), pipeline: [{$sort: {t: 1}}], as: 'result'}}];
    const res = localColl.aggregate(pipeline).toArray();
    assert.eq(res.length, 1);
}

runTest(beforeEpochDocs);
runTest(afterEpochDocs);
runTest(allExtendedRangeDocs);
runTest(mixedDocs);
