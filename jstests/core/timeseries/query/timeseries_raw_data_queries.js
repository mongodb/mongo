/**
 * Performs basic read operations on a time-series buckets collection through its time-series view
 * namespace using rawData.
 * @tags: [
 *   requires_timeseries,
 *   featureFlagRawDataCrudOperations,
 *   does_not_support_transactions,
 *   known_query_shape_computation_problem,  # TODO (SERVER-103069): Remove this tag.
 * ]
 */

// Set up some testing buckets to work with
const timeField = "t";
const metaField = "m";
const t = new Date("2002-05-29T00:00:00Z");

const coll = db[jsTestName()];

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
assert.commandWorked(coll.insertMany([
    {[timeField]: t, [metaField]: "1", v: "replacement"},
    {[timeField]: t, [metaField]: "2", v: "baz"},
    {[timeField]: t, [metaField]: "2", v: "qux"},
]));

assert(coll.drop());

function crudTest(fn, addStartingMeasurements = true) {
    db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    if (addStartingMeasurements) {
        assert.commandWorked(coll.insertMany([
            {[timeField]: t, [metaField]: "1", v: "foo"},
            {[timeField]: t, [metaField]: "1", v: "bar"},
            {[timeField]: t, [metaField]: "2", v: "baz"},
        ]));
    }
    fn();
    assert(coll.drop());
}

// aggregate()
crudTest(() => {
    const agg = coll.aggregate(
        [
            {$match: {"control.count": 2}},
        ],
        {rawData: true});
    assert.eq(agg.toArray().length, 1);

    assert.eq(coll.aggregate([{$indexStats: {}}, {$match: {name: "m_1_t_1"}}], {rawData: true})
                  .toArray()[0]
                  .key,
              {meta: 1, "control.min.t": 1, "control.max.t": 1});
});

// count()
crudTest(() => {
    assert.eq(coll.count({"control.count": 2}, {rawData: true}), 1);
});

// countDocuments()
crudTest(() => {
    assert.eq(coll.countDocuments({"control.count": 2}, {rawData: true}), 1);
});

// distinct()
crudTest(() => {
    assert.eq(coll.distinct("control.count", {}, {rawData: true}).sort(), [1, 2]);
});

// find()
crudTest(() => {
    assert.eq(coll.find().rawData().length(), 2);
    assert.eq(coll.find({"control.count": 2}).rawData().length(), 1);
});

// findOne()
crudTest(() => {
    const retrievedBucket =
        coll.findOne({"control.count": 2}, null, null, null, null, true /* rawData */);
    assert.eq(retrievedBucket.control.count, 2);
    assert.eq(retrievedBucket.meta, "1");
});
