/**
 * Tests a find command with rawData that internally executes a getMore.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagRawDataCrudOperations,
 *   does_not_support_transactions,
 *   # Refusing to run a test that issues commands that may return different values after a failover
 *   does_not_support_stepdowns,
 *   known_query_shape_computation_problem,  # TODO (SERVER-103069): Remove this tag.
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const timeField = "t";
const metaField = "m";
const t = new Date("2002-05-29T00:00:00Z");

const originalBatchSize = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryFindCommandBatchSize: 1}))["internalQueryFindCommandBatchSize"];

const coll = db[jsTestName()];

function setBatchSize(size) {
    FixtureHelpers.mapOnEachShardNode({
        db: db,
        func: function(db) {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryFindCommandBatchSize: size,
            }));
        },
    });
}

const runCrudTest = () => {
    db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    assert.commandWorked(coll.insertMany([
        {[timeField]: t, [metaField]: "1", v: "foo"},
        {[timeField]: t, [metaField]: "1", v: "bar"},
        {[timeField]: t, [metaField]: "2", v: "baz"},
    ]));
    assert.eq(coll.find().rawData().length(), 2);
    assert.eq(coll.find({"control.count": 2}).rawData().length(), 1);
    assert(coll.drop());
};

try {
    // Ensure that the find internally issues a getMore.
    setBatchSize(1);
    runCrudTest();
} finally {
    setBatchSize(originalBatchSize);
}
