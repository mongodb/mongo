/**
 * Tests that the join optimizer does not apply to unsupported collection types.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_replication,
 *   requires_sbe,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {featureFlagPathArrayness: true, internalEnableJoinOptimization: true},
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");

// Baseline "regular" collection used as the other side of each join.
const regular = testDB.regular;
regular.drop();
assert.commandWorked(
    regular.insertMany([
        {_id: 1, a: 1, b: 1},
        {_id: 2, a: 2, b: 2},
    ]),
);

// Provide multikeyness info for path arrayness.
assert.commandWorked(regular.createIndex({dummy: 1, a: 1, b: 1}));

function assertJoinOptNotUsed(coll, pipeline, msg) {
    const explain = coll.explain().aggregate(pipeline);
    assert(
        !joinOptUsed(explain),
        `${msg}: expected join optimizer was used unexpectedly: ` + tojson(explain),
    );
}

function assertJoinOptUsed(coll, pipeline, msg) {
    const explain = coll.explain().aggregate(pipeline);
    assert(joinOptUsed(explain), `${msg}: expected join optimizer to be used: ` + tojson(explain));
}

function lookupPipeline(foreignName) {
    return [
        {$lookup: {from: foreignName, localField: "a", foreignField: "a", as: "joined"}},
        {$unwind: "$joined"},
    ];
}

// Test a given "ineligible" collection both as the local side and the foreign side of a join.
function testIneligibleCollection({coll, label}) {
    // Foreign side.
    assertJoinOptNotUsed(regular, lookupPipeline(coll.getName()), `${label} as foreign side`);

    // Local side.
    assertJoinOptNotUsed(coll, lookupPipeline(regular.getName()), `${label} as local side`);
}

// Timeseries collection is ineligible.
{
    const tsName = "ts_coll";
    testDB[tsName].drop();
    // Note: fields "t" and "a" should both be indexed due to the nature of timeseries queries.
    assert.commandWorked(
        testDB.createCollection(tsName, {timeseries: {timeField: "t", metaField: "a"}}),
    );
    const ts = testDB[tsName];
    assert.commandWorked(
        ts.insert([
            {t: new Date(), a: 1, b: 1},
            {t: new Date(), a: 2, b: 2},
        ]),
    );
    testIneligibleCollection({coll: ts, label: "timeseries"});
}

// Clustered collection is ineligible.
{
    const clusteredName = "clustered_coll";
    testDB[clusteredName].drop();
    assert.commandWorked(
        testDB.createCollection(clusteredName, {
            clusteredIndex: {key: {_id: 1}, unique: true},
        }),
    );
    const clustered = testDB[clusteredName];
    assert.commandWorked(
        clustered.insertMany([
            {_id: 1, a: 1, b: 1},
            {_id: 2, a: 2, b: 2},
        ]),
    );
    assert.commandWorked(clustered.createIndex({dummy: 1, a: 1, b: 1}));
    testIneligibleCollection({coll: clustered, label: "clustered"});
}

// Capped collection is ineligible.
{
    const cappedName = "capped_coll";
    testDB[cappedName].drop();
    assert.commandWorked(testDB.createCollection(cappedName, {capped: true, size: 4096}));
    const capped = testDB[cappedName];
    assert.commandWorked(
        capped.insertMany([
            {a: 1, b: 1},
            {a: 2, b: 2},
        ]),
    );
    assert.commandWorked(capped.createIndex({dummy: 1, a: 1, b: 1}));
    testIneligibleCollection({coll: capped, label: "capped"});
}

// Collection with a non-simple collation is ineligible.
{
    const collationName = "collation_coll";
    testDB[collationName].drop();
    assert.commandWorked(
        testDB.createCollection(collationName, {collation: {locale: "en_US", strength: 2}}),
    );
    const collated = testDB[collationName];
    assert.commandWorked(
        collated.insertMany([
            {a: 1, b: 1},
            {a: 2, b: 2},
        ]),
    );
    assert.commandWorked(collated.createIndex({dummy: 1, a: 1, b: 1}));
    testIneligibleCollection({coll: collated, label: "collation"});
}

// Ensure views can't participate in join-opt.
// TODO SERVER-112239: permit views/ resolve them.
{
    const viewName = "view_coll";
    const view = testDB[viewName];
    view.drop();
    assert.commandWorked(
        testDB.createView(viewName, regular.getName(), [{$match: {a: {$gte: 0}}}]),
    );

    // Can't use a view as a foreign collection.
    assertJoinOptNotUsed(regular, lookupPipeline(viewName), "foreign coll is a view");

    // CAN use a view as a local collection, provided the view is a prefix we can handle.
    const pipeline = lookupPipeline(regular.getName());
    assertJoinOptUsed(view, pipeline, "local coll is a view");
    assertArrayEq({
        expected: [
            {
                "_id": 1,
                "a": 1,
                "b": 1,
                "joined": {
                    "_id": 1,
                    "a": 1,
                    "b": 1,
                },
            },
            {
                "_id": 2,
                "a": 2,
                "b": 2,
                "joined": {
                    "_id": 2,
                    "a": 2,
                    "b": 2,
                },
            },
        ],
        actual: view.aggregate(pipeline).toArray(),
    });

    // Again, even if it uses extra $lookups!
    assert.commandWorked(
        testDB.createView("lookupView", regular.getName(), [
            {$lookup: {from: regular.getName(), localField: "b", foreignField: "b", as: "j2"}},
            {$unwind: "$j2"},
        ]),
    );
    assertJoinOptUsed(testDB["lookupView"], pipeline, "local coll is a view with joins");
    assertArrayEq({
        expected: [
            {
                "_id": 1,
                "a": 1,
                "b": 1,
                "joined": {
                    "_id": 1,
                    "a": 1,
                    "b": 1,
                },
                "j2": {
                    "_id": 1,
                    "a": 1,
                    "b": 1,
                },
            },
            {
                "_id": 2,
                "a": 2,
                "b": 2,
                "joined": {
                    "_id": 2,
                    "a": 2,
                    "b": 2,
                },
                "j2": {
                    "_id": 2,
                    "a": 2,
                    "b": 2,
                },
            },
        ],
        actual: testDB["lookupView"].aggregate(pipeline).toArray(),
    });

    // But not if it is an ineligible pipeline prefix.
    assert.commandWorked(
        testDB.createView("ineligibleView", regular.getName(), [{$group: {_id: "$a"}}]),
    );
    testIneligibleCollection({coll: testDB["ineligibleView"], label: "ineligible view"});
}

// Oplog collection (local.oplog.rs) should not be eligible.
{
    const localDB = primary.getDB("local");
    const oplog = localDB.getCollection("oplog.rs");
    // Create a non-oplog collection in the "local" DB that is a valid local side, and join against
    // the oplog as the foreign side within the same DB.
    const localRegular = localDB.join_opt_local_regular;
    localRegular.drop();
    assert.commandWorked(
        localRegular.insertMany([
            {a: 1, b: 1},
            {a: 2, b: 2},
        ]),
    );
    assert.commandWorked(localRegular.createIndex({dummy: 1, a: 1, b: 1}));
    testIneligibleCollection({coll: oplog, label: "oplog"});
}

rst.stopSet();
