/**
 * Test that internal systems can run an explicit $_unpackBucket operation directly against a
 * time-series bucket collection. This stage is only used for special known use cases by other
 * MongoDB products rather than user applications.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const tsCollWithMeta = testDB.getCollection("tsCollWithMeta");
const tsCollWithoutMeta = testDB.getCollection("tsCollWithoutMeta");
assert.commandWorked(testDB.createCollection(
    tsCollWithMeta.getName(), {timeseries: {timeField: "start", metaField: "tags"}}));
assert.commandWorked(
    testDB.createCollection(tsCollWithoutMeta.getName(), {timeseries: {timeField: "start"}}));

const nMeasurements = 50;
const aDayInMs = 24 * 60 * 60 * 1000;
const seedDate = new Date("2020-11-30T12:10:05Z");
const mid = nMeasurements / 2;
const midDate = new Date(seedDate.valueOf() + mid * aDayInMs);

for (let i = 0; i < nMeasurements; i++) {
    const docToInsert = {
        start: new Date(seedDate.valueOf() + i * aDayInMs),
        end: new Date(seedDate.valueOf() + (i + 1) * aDayInMs),
        tags: "cpu",
        value: i + nMeasurements,
    };
    assert.commandWorked(tsCollWithMeta.insert(docToInsert));
    assert.commandWorked(tsCollWithoutMeta.insert(docToInsert));
}

const sysCollWithMeta = testDB.getCollection("system.buckets." + tsCollWithMeta.getName());
const sysCollWithoutMeta = testDB.getCollection("system.buckets." + tsCollWithoutMeta.getName());

const resultsWithMeta = sysCollWithMeta
                            .aggregate([
                                {$_unpackBucket: {timeField: "start", metaField: "tags"}},
                                {$project: {_id: 0}},
                                {$sort: {value: 1}}
                            ])
                            .toArray();
const resultsWithoutMeta =
    sysCollWithoutMeta
        .aggregate(
            [{$_unpackBucket: {timeField: "start"}}, {$project: {_id: 0}}, {$sort: {value: 1}}])
        .toArray();
const resultsWithNonExistentMeta = sysCollWithoutMeta
                                       .aggregate([
                                           {$_unpackBucket: {timeField: "start", metaField: "foo"}},
                                           {$project: {_id: 0}},
                                           {$sort: {value: 1}}
                                       ])
                                       .toArray();
// Test that $_unpackBucket + $match can work as expected. The $match will be rewritten and pushed
// down to be ahead of $_unpackBucket.
const resultsFilteredByTime = sysCollWithMeta
                                  .aggregate([
                                      {$_unpackBucket: {timeField: "start", metaField: "tags"}},
                                      {$match: {start: {$lt: midDate}}},
                                      {$project: {_id: 0}},
                                      {$sort: {value: 1}}
                                  ])
                                  .toArray();
// Get the result from directly querying the time-series collection and compare with implicit
// bucket unpacking.
const expectedResult = tsCollWithMeta.find({}, {_id: 0}).sort({value: 1}).toArray();
assert.eq(nMeasurements, expectedResult.length, expectedResult);
assert.eq(nMeasurements, resultsWithMeta.length, resultsWithMeta);
assert.eq(nMeasurements, resultsWithoutMeta.length, resultsWithoutMeta);
assert.eq(nMeasurements, resultsWithNonExistentMeta.length, resultsWithNonExistentMeta);
assert.eq(mid, resultsFilteredByTime.length, resultsFilteredByTime);
for (let i = 0; i < nMeasurements; i++) {
    const expected = {
        start: new Date(seedDate.valueOf() + i * aDayInMs),
        end: new Date(seedDate.valueOf() + (i + 1) * aDayInMs),
        tags: "cpu",
        value: i + nMeasurements,
    };
    assert.docEq(expected, expectedResult[i], expectedResult);
    assert.docEq(expected, resultsWithMeta[i], resultsWithMeta);
    assert.docEq(expected, resultsWithoutMeta[i], resultsWithoutMeta);
    assert.docEq(expected, resultsWithNonExistentMeta[i], resultsWithNonExistentMeta);
    if (i < mid) {
        assert.docEq(expected, resultsFilteredByTime[i], resultsFilteredByTime);
    }
}

// $_unpackBucket fails if parameters other than "timeField" and "metaField" are specified.
// "include"
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([
                     {$_unpackBucket: {include: ["start"], timeField: "start", metaField: "tags"}}
                 ])),
                 5612404);
// "exclude"
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([{
                     $_unpackBucket:
                         {exclude: ["start", "value"], timeField: "start", metaField: "tags"}
                 }])),
                 5612404);
// "bucketMaxSpanSeconds"
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([
                     {
                         $_unpackBucket:
                             {timeField: "start", metaField: "tags", bucketMaxSpanSeconds: 3000}
                     },
                 ])),
                 5612404);
// "computedMetaProjFields"
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([{
                     $_unpackBucket:
                         {timeField: "start", metaField: "tags", computedMetaProjFields: ["foo"]}
                 }])),
                 5612404);
// "foo"
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithoutMeta.aggregate(
                      [{$_unpackBucket: {timeField: "start", foo: 1024}}])),
                 5612404);

// $_unpackBucket specification must be an object.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([{$_unpackBucket: "foo"}])), 5612400);
// $_unpackBucket fails if "timeField" is not a string.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([{$_unpackBucket: {timeField: 60}}])), 5612401);
// $_unpackBucket fails if "metaField" is not a string.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate(
                      [{$_unpackBucket: {timeField: "time", metaField: 250}}])),
                 5612402);
// $_unpackBucket fails if "metaField" is not a a single-element field path.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate(
                      [{$_unpackBucket: {timeField: "time", metaField: "a.b.c"}}])),
                 5612403);
// $_unpackBucket fails if timeField is not specified.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([{$_unpackBucket: {metaField: "tags"}}])),
                 5612405);
// $_unpackBucket fails if the time-series bucket collection has a metaField but it was not
// included as a parameter.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate([{$_unpackBucket: {timeField: "start"}}])),
                 5369601);

// Collection creation fails if 'timeField' or 'metaField' contains embedded null bytes.
const tsCollNullByte = testDB.getCollection("tsCollNullByte");
assert.commandFailedWithCode(
    assert.throws(() => assert.commandWorked(testDB.createCollection(
                      tsCollNullByte.getName(),
                      {timeseries: {timeField: "a\x00b", metaField: "tags"}}))),
                 ErrorCodes.BadValue);

assert.commandFailedWithCode(
    assert.throws(() => assert.commandWorked(testDB.createCollection(
                      tsCollNullByte.getName(),
                      {timeseries: {timeField: "time", metaField: "a\x00b"}}))),
                 ErrorCodes.BadValue);

// $_unpackBucket fails if timeField or metaField contains null bytes.
assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate(
                      [{$_unpackBucket: {timeField: "a\x00b", metaField: "tags"}}])),
                 9568703);

assert.commandFailedWithCode(
    assert.throws(() => sysCollWithMeta.aggregate(
                      [{$_unpackBucket: {timeField: "time", metaField: "a\x00b"}}])),
                 9568704);

// $_internalUnpackBucket fails if timeField or metaField contains null bytes.
assert.commandFailedWithCode(assert.throws(() => sysCollWithMeta.aggregate([{
                                              $_internalUnpackBucket: {
                                                  timeField: "a\x00b",
                                                  metaField: "tags",
                                                  bucketMaxSpanSeconds: NumberInt(3600)
                                              }
                                          }])),
                                          9568701);

assert.commandFailedWithCode(assert.throws(() => sysCollWithMeta.aggregate([{
                                              $_internalUnpackBucket: {
                                                  timeField: "time",
                                                  metaField: "a\x00b",
                                                  bucketMaxSpanSeconds: NumberInt(3600)
                                              }
                                          }])),
                                          9568702);

// $_internalUnpackBucket fails if include or exclude contains null bytes.
// "include"
assert.commandFailedWithCode(assert.throws(() => sysCollWithMeta.aggregate([{
                                              $_internalUnpackBucket: {
                                                  include: ["start", "invalid_\x00"],
                                                  timeField: "start",
                                                  metaField: "tags",
                                                  bucketMaxSpanSeconds: NumberInt(3600)
                                              }
                                          }])),
                                          9568705);
// "exclude"
assert.commandFailedWithCode(assert.throws(() => sysCollWithMeta.aggregate([{
                                              $_internalUnpackBucket: {
                                                  exclude: ["start", "value_\x00"],
                                                  timeField: "start",
                                                  metaField: "tags",
                                                  bucketMaxSpanSeconds: NumberInt(3600)
                                              }
                                          }])),
                                          9568705);

// $_internalUnpackBucket fails if computedMetaProjFields contains null bytes.
assert.commandFailedWithCode(assert.throws(() => sysCollWithMeta.aggregate([{
                                              $_internalUnpackBucket: {
                                                  timeField: "time",
                                                  metaField: "tags",
                                                  computedMetaProjFields: ["invalid_\x00_field"]
                                              }
                                          }])),
                                          9568706);
