/**
 * Timeseries collections with $-prefixed timeseries fields were disallowed in 8.3
 *
 * This test verifies that creating these collections before and during an upgrade works, and the
 * collections stay queryable after an upgrade, but new collections cannot be created.
 *
 * Reproduces an issue (see BF-40846) where creating a collection in a mixed-version cluster could cause an
 * error when applying the oplog on a secondary.
 *
 * TODO(SERVER-117054): Remove the upgrade part of the test once 9.0 is last-lts. After that this test
 * should just check that $-prefixed timeseries collections can't be created.
 */

import {testPerformReplSetRollingRestart} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

const inputCollTimeName = jsTestName() + "_input_1";
const inputCollMetaName = jsTestName() + "_input_2";

const dollarTimeValues = [
    {"$t": ISODate(), m: 1.5, _id: 1},
    {"$t": ISODate(), m: 2.5, _id: 2},
];

const dollarMetaValues = [
    {t: ISODate(), "$m": 3.5, _id: 1},
    {t: ISODate(), "$m": 4.5, _id: 2},
];

function setUp(primaryConnection) {
    // Set up regular input collections for $out queries
    const db = getDB(primaryConnection);

    const inputCollTime = db[inputCollTimeName];
    inputCollTime.drop();
    assert.commandWorked(inputCollTime.insertMany(dollarTimeValues));

    const inputCollMeta = db[inputCollMetaName];
    inputCollMeta.drop();
    assert.commandWorked(inputCollMeta.insertMany(dollarMetaValues));
}

function createDollarTimeCollection(db, collName) {
    return db.createCollection(collName, {timeseries: {timeField: "$t"}});
}

function createDollarMetaCollection(db, collName) {
    return db.createCollection(collName, {timeseries: {timeField: "t", metaField: "$m"}});
}

function createOutCollection(db, inputCollName, outputCollName, timeseriesOptions) {
    return db.runCommand({
        aggregate: inputCollName,
        pipeline: [
            {
                $out: {
                    db: db.getName(),
                    coll: outputCollName,
                    timeseries: timeseriesOptions,
                },
            },
        ],
        cursor: {},
    });
}

function createOutDollarTimeCollection(db, collName) {
    return createOutCollection(db, inputCollTimeName, collName, {timeField: "$t"});
}

function createOutDollarMetaCollection(db, collName) {
    return createOutCollection(db, inputCollMetaName, collName, {timeField: "t", metaField: "$m"});
}

function assertCreateDollarCollectionsWorks(db) {
    // Bad timefield, no metaField
    const coll1 = db[jsTestName() + "_1"];
    coll1.drop();
    assert.commandWorked(createDollarTimeCollection(db, coll1.getName()));
    assert.commandWorked(coll1.insertMany(dollarTimeValues));

    // Good timeField, bad metaField
    const coll2 = db[jsTestName() + "_2"];
    coll2.drop();
    assert.commandWorked(createDollarMetaCollection(db, coll2.getName()));
    assert.commandWorked(coll2.insertMany(dollarMetaValues));

    // $out with bad timefield
    const outCollName1 = jsTestName() + "_out_1";
    assert.commandWorked(createOutDollarTimeCollection(db, outCollName1));

    // $out with bad metafield
    const outCollName2 = jsTestName() + "_out_2";
    assert.commandWorked(createOutDollarMetaCollection(db, outCollName2));
}

function assertCreateDollarCollectionsFails(db) {
    const collName = jsTestName() + "_failed";

    // Bad timefield, no metaField
    assert.commandFailedWithCode(createDollarTimeCollection(db, collName), ErrorCodes.BadValue);
    // Good timeField, bad metaField
    assert.commandFailedWithCode(createDollarMetaCollection(db, collName), ErrorCodes.BadValue);
    // $out with bad timefield
    assert.commandFailedWithCode(createOutDollarTimeCollection(db, collName), ErrorCodes.BadValue);
    // $out with bad metafield
    assert.commandFailedWithCode(createOutDollarMetaCollection(db, collName), ErrorCodes.BadValue);

    // note: these cases aren't checked before the upgrade because they already failed before 8.3,
    // but check that they error as expected

    // Bad timeField, good metaField.
    assert.commandFailedWithCode(
        db.createCollection(collName, {timeseries: {timeField: "$t", metaField: "m"}}),
        ErrorCodes.BadValue,
    );

    // Bad timeField and metaField.
    assert.commandFailedWithCode(
        db.createCollection(collName, {timeseries: {timeField: "$t", metaField: "$m"}}),
        ErrorCodes.BadValue,
    );
}

function assertCollectionsQueryable(db) {
    const coll1 = db[jsTestName() + "_1"];
    assertArrayEq({actual: coll1.find().toArray(), expected: dollarTimeValues});
    const coll2 = db[jsTestName() + "_2"];
    assertArrayEq({actual: coll2.find().toArray(), expected: dollarMetaValues});
    const outColl1 = db[jsTestName() + "_out_1"];
    assertArrayEq({actual: outColl1.find().toArray(), expected: dollarTimeValues});
    const outColl2 = db[jsTestName() + "_out_2"];
    assertArrayEq({actual: outColl2.find().toArray(), expected: dollarMetaValues});
}

testPerformReplSetRollingRestart({
    // support for $-prefixed timeField/metaField was removed in 8.3
    startingVersion: {binVersion: "last-lts"},
    setupFn: setUp,
    beforeRestart: () => {},
    afterSecondariesHaveRestarted: (primaryConnection) => {
        const db = getDB(primaryConnection);

        // Create collections while secondaries are on new version and primary is on old version
        assertCreateDollarCollectionsWorks(db);

        // check that collections are queryable
        assertCollectionsQueryable(db);
    },
    afterPrimariesHaveRestarted: (primaryConnection) => {
        const db = getDB(primaryConnection);

        // check that collections are still queryable
        assertCollectionsQueryable(db);

        // check that new collections can no longer be created
        assertCreateDollarCollectionsFails(db);
    },
    afterFCVBump: (primaryConnection) => {
        const db = getDB(primaryConnection);

        // check that collections are still queryable
        assertCollectionsQueryable(db);

        // check that new collections can no longer be created
        assertCreateDollarCollectionsFails(db);
    },
});
