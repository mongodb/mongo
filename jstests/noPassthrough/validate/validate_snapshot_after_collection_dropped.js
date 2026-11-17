/**
 * Tests that 'validate' at a snapshot validates a collection that existed at that snapshot but is
 * no longer in the latest catalog: it was dropped, its database was dropped, it was renamed away,
 * or a collection of the other type took its name. Covers foreground validation with
 * 'atClusterTime' and background validation, which reads the last checkpoint, on the primary and
 * on a secondary.
 *
 * @tags: [
 *   requires_fsync,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("Validation at a snapshot of a collection that is gone from the latest catalog", function () {
    const collName = "coll";
    let dbCount = 0;

    function startReplSet(nodeOptions) {
        const rst = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
            nodeOptions,
        });
        rst.startSet();
        rst.initiate();
        rst.getSecondary().setSecondaryOk();
        return rst;
    }

    function createWithData(db, timeseries) {
        const options = timeseries ? {timeseries: {timeField: "t", metaField: "m"}} : {};
        assert.commandWorked(db.createCollection(collName, options));
        const docs = [];
        for (let i = 0; i < 20; i++) {
            docs.push({t: ISODate("2026-02-10T18:31:35.788Z"), m: i % 3, a: i});
        }
        assert.commandWorked(db[collName].insertMany(docs));
        assert.commandWorked(db[collName].createIndex({a: 1}));
    }

    // Returns what a validation of the collection as it is now reports.
    function validateNow(db) {
        const res = assert.commandWorked(db.runCommand({validate: collName}));
        assert(res.valid, "live collection is not valid", {res});
        return {uuid: res.uuid, nrecords: res.nrecords, nIndexes: res.nIndexes};
    }

    // Checks that the latest catalog on 'conn' no longer has the collection, or has one of the
    // other type when the scenario reuses the name.
    function assertGone(conn, dbName, scenario, timeseries) {
        const infos = conn.getDB(dbName).getCollectionInfos({name: collName});
        const typeNow = infos.length ? infos[0].type : null;
        let expectedType = null;
        if (scenario.reusesName) {
            expectedType = timeseries ? "collection" : "timeseries";
        }
        assert.eq(typeNow, expectedType, "unexpected collection in the latest catalog");
    }

    function assertValidatedSnapshot(res, expected) {
        assert.commandWorked(res);
        assert(res.valid, "validation at the snapshot found the collection invalid", {res});
        assert.eq(res.errors.length, 0, "validation at the snapshot reported errors", {res});
        assert.eq(res.uuid, expected.uuid, "validated a different collection", {res});
        assert.eq(res.nrecords, expected.nrecords, "wrong record count", {res});
        assert.eq(res.nIndexes, expected.nIndexes, "wrong index count", {res});
    }

    const scenarios = [
        {
            name: "the collection was dropped",
            remove: (db) => assert(db[collName].drop()),
        },
        {
            name: "the database was dropped",
            remove: (db) => assert.commandWorked(db.dropDatabase()),
        },
        {
            name: "the collection was renamed",
            remove: (db) =>
                assert.commandWorked(
                    db.adminCommand({
                        renameCollection: `${db.getName()}.${collName}`,
                        to: `${db.getName()}.${collName}_renamed`,
                    }),
                ),
        },
        {
            name: "the collection was dropped, on a secondary",
            remove: (db) => assert(db[collName].drop()),
            onSecondary: true,
        },
        {
            name: "a collection of the other type took its name",
            remove: (db, timeseries) => {
                assert(db[collName].drop());
                createWithData(db, !timeseries);
            },
            reusesName: true,
        },
    ];

    describe("foreground validation with atClusterTime", function () {
        let rst;

        before(function () {
            rst = startReplSet({});
        });

        after(function () {
            rst.stopSet();
        });

        for (const timeseries of [false, true]) {
            for (const scenario of scenarios) {
                const type = timeseries ? "timeseries" : "regular";
                it(`validates a ${type} collection after ${scenario.name}`, function () {
                    const primary = rst.getPrimary();
                    const conn = scenario.onSecondary ? rst.getSecondary() : primary;
                    const db = primary.getDB(`${jsTestName()}_${++dbCount}`);
                    const dbName = db.getName();

                    createWithData(db, timeseries);
                    const expected = validateNow(db);
                    rst.awaitReplication();
                    const atClusterTime = assert.commandWorked(
                        primary.adminCommand({replSetGetStatus: 1}),
                    ).optimes.appliedOpTime.ts;

                    scenario.remove(db, timeseries);
                    rst.awaitReplication();
                    assertGone(conn, dbName, scenario, timeseries);

                    const res = conn.getDB(dbName).runCommand({validate: collName, atClusterTime});
                    assertValidatedSnapshot(res, expected);
                });
            }
        }
    });

    describe("background validation", function () {
        let rst;

        before(function () {
            // Stop the checkpoint thread, so the only checkpoints are the ones this test takes with
            // fsync before it removes the collection. Background validation reads the last
            // checkpoint.
            rst = startReplSet({syncdelay: 0});
        });

        after(function () {
            rst.stopSet();
        });

        for (const timeseries of [false, true]) {
            for (const scenario of scenarios) {
                const type = timeseries ? "timeseries" : "regular";
                it(`validates a ${type} collection after ${scenario.name}`, function () {
                    const primary = rst.getPrimary();
                    const conn = scenario.onSecondary ? rst.getSecondary() : primary;
                    const db = primary.getDB(`${jsTestName()}_${++dbCount}`);
                    const dbName = db.getName();

                    createWithData(db, timeseries);
                    const expected = validateNow(db);
                    rst.awaitReplication();

                    // fsync checkpoints at the node's stable timestamp, which can lag its last
                    // applied write. Retry until a background validation sees the data and
                    // indexes. 'nrecords' and 'nIndexes' are int32 in the reply, so they compare
                    // as numbers. 'state' is logged as the attr of a failed assert.soon.
                    const state = {expected, last: null};
                    assert.soon(
                        () => {
                            assert.commandWorked(conn.adminCommand({fsync: 1}));
                            const res = conn
                                .getDB(dbName)
                                .runCommand({validate: collName, background: true});
                            state.last = res;
                            return (
                                res.ok === 1 &&
                                res.nrecords === expected.nrecords &&
                                res.nIndexes === expected.nIndexes
                            );
                        },
                        "checkpoint does not hold the collection with its data and indexes",
                        undefined,
                        undefined,
                        {},
                        state,
                    );

                    scenario.remove(db, timeseries);
                    rst.awaitReplication();
                    assertGone(conn, dbName, scenario, timeseries);

                    const res = conn
                        .getDB(dbName)
                        .runCommand({validate: collName, background: true});
                    assertValidatedSnapshot(res, expected);
                });
            }
        }
    });
});
