/**
 * Tests the correctness of basic fields in write command slow query logs on a standalone mongod:
 *   - collectionType, ns, nreturned, queryShapeHash
 *   - write metrics (ninserted, nMatched, nModified, ndeleted)
 * @tags: [
 *  requires_fcv_90,
 * ]
 *
 * The slow query logs are triggered via `setProfilingLevel(0, -1)`, which logs every operation as
 * a "Slow query".
 *
 * Write commands are logged twice: once as a top-level command entry (whose `ns` is `<db>.$cmd`)
 * and once as an "op" entry (`type: "update"` / `type: "remove"`) that carries the collection
 * namespace, the write metrics, and the queryShapeHash. Tests for update/delete therefore match on
 * `type` to select the op entry. `findAndModify` and `insert` produce a single entry.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    checkCollectionType,
    checkWriteFields,
} from "jstests/noPassthrough/logging/log_collectionType_utils.js";

const dbName = jsTestName();
const collName = "testColl";
const tsCollName = "tsColl";

const comments = {
    insertColl: "write_insert_coll",
    updateMulti: "write_update_multi",
    updateSingle: "write_update_single",
    updatePipeline: "write_update_pipeline",
    deleteMulti: "write_delete_multi",
    deleteSingle: "write_delete_single",
    findAndModifyUpdate: "write_findAndModify_update",
    findAndModifyRemove: "write_findAndModify_remove",
    bulkWrite: "write_bulkWrite",
    tsInsert: "write_insert_timeseries",
    tsUpdate: "write_update_timeseries",
};

const fullNs = `${dbName}.${collName}`;
const tsFullNs = `${dbName}.${tsCollName}`;

// Each document has a value from a small domain with one duplicate so that nMatched/nModified and
// ndeleted counts are deterministic and distinguishable from the number of documents.
function resetColl(db) {
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 2}]));
}

function setupTimeseries(db) {
    assert.commandWorked(
        db.createCollection(tsCollName, {
            timeseries: {timeField: "timestamp", metaField: "metadata"},
        }),
    );
    assert.commandWorked(
        db[tsCollName].insertMany([
            {metadata: {m: 1}, timestamp: ISODate("2021-05-18T00:00:00.000Z"), a: 1},
            {metadata: {m: 1}, timestamp: ISODate("2021-05-18T00:00:01.000Z"), a: 2},
            {metadata: {m: 1}, timestamp: ISODate("2021-05-18T00:00:02.000Z"), a: 2},
        ]),
    );
}

describe("write command slow query logs", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {internalQueryStatsWriteCmdSampleRate: 0},
        });
        this.db = this.conn.getDB(dbName);
        this.db.setProfilingLevel(0, -1);
        resetColl(this.db);
        setupTimeseries(this.db);
    });
    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("insert logs ns, collectionType, ninserted and queryShapeHash", function () {
        const comment = comments.insertColl;
        assert.commandWorked(this.db.runCommand({insert: collName, documents: [{a: 10}], comment}));
        checkWriteFields({
            db: this.db,
            comment,
            command: "insert",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {ninserted: 1},
        });
        checkCollectionType({
            db: this.db,
            comment,
            command: "insert",
            expectedCollType: "normal",
        });
    });

    it("update (multi) logs ns, collectionType, nModified and queryShapeHash", function () {
        resetColl(this.db);
        const comment = comments.updateMulti;
        const res = assert.commandWorked(
            this.db.runCommand({
                update: collName,
                updates: [{q: {a: {$gt: 0}}, u: {$set: {b: 1}}, multi: true}],
                comment,
            }),
        );
        assert.eq(3, res.n); // The update command reply reports the matched count as `n`.
        assert.eq(3, res.nModified);
        checkWriteFields({
            db: this.db,
            comment,
            type: "update",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {nMatched: 3, nModified: 3},
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "update",
            expectedCollType: "normal",
        });
    });

    it("update (single) logs ns, collectionType, nModified and queryShapeHash", function () {
        resetColl(this.db);
        const comment = comments.updateSingle;
        const res = assert.commandWorked(
            this.db.runCommand({
                update: collName,
                updates: [{q: {a: 1}, u: {$set: {b: 2}}}],
                comment,
            }),
        );
        assert.eq(1, res.n); // The update command reply reports the matched count as `n`.
        assert.eq(1, res.nModified);
        checkWriteFields({
            db: this.db,
            comment,
            type: "update",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {nMatched: 1, nModified: 1},
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "update",
            expectedCollType: "normal",
        });
    });

    it("update (pipeline) logs ns, collectionType, nModified and queryShapeHash", function () {
        resetColl(this.db);
        const comment = comments.updatePipeline;
        const res = assert.commandWorked(
            this.db.runCommand({
                update: collName,
                updates: [{q: {a: {$gt: 0}}, u: [{$set: {c: 1}}], multi: true}],
                comment,
            }),
        );
        assert.eq(3, res.n); // The update command reply reports the matched count as `n`.
        assert.eq(3, res.nModified);
        checkWriteFields({
            db: this.db,
            comment,
            type: "update",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {nMatched: 3, nModified: 3},
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "update",
            expectedCollType: "normal",
        });
    });

    it("delete (multi) logs ns, collectionType, ndeleted and queryShapeHash", function () {
        resetColl(this.db);
        const comment = comments.deleteMulti;
        const res = assert.commandWorked(
            this.db.runCommand({
                delete: collName,
                deletes: [{q: {a: 2}, limit: 0}],
                comment,
            }),
        );
        assert.eq(2, res.n);
        checkWriteFields({
            db: this.db,
            comment,
            type: "remove",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {ndeleted: 2},
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "remove",
            expectedCollType: "normal",
        });
    });

    it("delete (single) logs ns, collectionType, ndeleted and queryShapeHash", function () {
        resetColl(this.db);
        const comment = comments.deleteSingle;
        const res = assert.commandWorked(
            this.db.runCommand({
                delete: collName,
                deletes: [{q: {a: 2}, limit: 1}],
                comment,
            }),
        );
        assert.eq(1, res.n);
        checkWriteFields({
            db: this.db,
            comment,
            type: "remove",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {ndeleted: 1},
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "remove",
            expectedCollType: "normal",
        });
    });

    // TODO SERVER-134826 findAndModify slow query logs currently omit queryShapeHash. The two
    // tests below assert its absence; see the disabled 'findAndModify logs queryShapeHash' test
    // that should be enabled once the follow-up lands.
    it("findAndModify (update) logs ns, collectionType and nreturned", function () {
        resetColl(this.db);
        const comment = comments.findAndModifyUpdate;
        assert.commandWorked(
            this.db.runCommand({
                findAndModify: collName,
                query: {a: 1},
                update: {$set: {b: 5}},
                new: true,
                comment,
            }),
        );
        checkWriteFields({
            db: this.db,
            comment,
            type: "command",
            expectedNs: fullNs,
            expectedNReturned: 1,
            expectQueryShapeHash: false,
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "command",
            expectedCollType: "normal",
        });
    });

    // TODO SERVER-134826 findAndModify slow query logs currently omit queryShapeHash.
    it("findAndModify (remove) logs ns, collectionType and nreturned", function () {
        resetColl(this.db);
        const comment = comments.findAndModifyRemove;
        assert.commandWorked(
            this.db.runCommand({
                findAndModify: collName,
                query: {a: 2},
                remove: true,
                comment,
            }),
        );
        checkWriteFields({
            db: this.db,
            comment,
            type: "command",
            expectedNs: fullNs,
            expectedNReturned: 1,
            expectQueryShapeHash: false,
        });
        checkCollectionType({
            db: this.db,
            comment,
            type: "command",
            expectedCollType: "normal",
        });
    });

    it("bulkWrite logs per-op entries with ns, collectionType and per-op metrics", function () {
        resetColl(this.db);
        const comment = comments.bulkWrite;
        const res = assert.commandWorked(
            this.db.getSiblingDB("admin").runCommand({
                bulkWrite: 1,
                ops: [
                    {insert: 0, document: {a: 20}},
                    {update: 1, filter: {a: 20}, updateMods: {$set: {b: 1}}},
                    {delete: 2, filter: {a: 20}},
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                comment,
            }),
        );
        assert.eq(1, res.nInserted);
        assert.eq(1, res.nMatched);
        assert.eq(1, res.nModified);
        assert.eq(1, res.nDeleted);

        // Note: bulkWrite ops log as a per-op entry whose `command` is keyed by the op index
        // (e.g. {"insert": 0, ...}), so they are matched by `command` even though the entries use
        // `type: "msg"`.
        // TODO SERVER-134827 bulkWrite op entries currently omit queryShapeHash. See the disabled
        // test 'bulkWrite ops log queryShapeHash' below.
        checkWriteFields({
            db: this.db,
            comment,
            command: "insert",
            expectedNs: fullNs,
            expectQueryShapeHash: false,
            expectedMetrics: {ninserted: 1},
        });
        checkCollectionType({
            db: this.db,
            comment,
            command: "insert",
            expectedCollType: "normal",
        });
        checkWriteFields({
            db: this.db,
            comment,
            command: "update",
            expectedNs: fullNs,
            expectQueryShapeHash: false,
            expectedMetrics: {nMatched: 1, nModified: 1},
        });
        checkWriteFields({
            db: this.db,
            comment,
            command: "delete",
            expectedNs: fullNs,
            expectQueryShapeHash: false,
            expectedMetrics: {ndeleted: 1},
        });
    });

    it.skip("FAM TODO SERVER-134826: findAndModify logs queryShapeHash", function () {
        // Current (incorrect) behavior: findAndModify slow query log entries omit queryShapeHash,
        // unlike insert/update/delete. See the enabled findAndModify tests above, which pin the
        // hash's absence.
        resetColl(this.db);
        const comment = comments.findAndModifyUpdate;
        assert.commandWorked(
            this.db.runCommand({
                findAndModify: collName,
                query: {a: 1},
                update: {$set: {b: 5}},
                new: true,
                comment,
            }),
        );
        checkWriteFields({
            db: this.db,
            comment,
            type: "command",
            expectedNs: fullNs,
            expectedNReturned: 1,
            expectQueryShapeHash: true,
        });
    });

    it.skip("TIMESERIES TODO SERVER-134824: timeseries insert logs collectionType 'timeseries'", function () {
        // Current (incorrect) behavior: OpDebug::collectionType is derived from the namespace
        // string for write commands, so an insert into a timeseries collection logs
        // collectionType "normal" instead of "timeseries". The query-stats key already computes
        // the correct query_shape::CollectionType (kTimeseries) for the insert; the follow-up
        // should surface that value in OpDebug for write commands.
        resetColl(this.db);
        const comment = comments.tsInsert;
        assert.commandWorked(
            this.db.runCommand({
                insert: tsCollName,
                documents: [
                    {metadata: {m: 2}, timestamp: ISODate("2021-05-18T00:00:03.000Z"), a: 9},
                ],
                comment,
            }),
        );
        checkCollectionType({
            db: this.db,
            comment,
            command: "insert",
            expectedCollType: "timeseries",
        });
    });

    it.skip("TIMESERIES TODO SERVER-134825: timeseries insert logs queryShapeHash", function () {
        // Current (incorrect) behavior: query shape registration (and therefore queryShapeHash in
        // the slow query log) is skipped for timeseries insert execution.
        resetColl(this.db);
        const comment = comments.tsInsert;
        assert.commandWorked(
            this.db.runCommand({
                insert: tsCollName,
                documents: [
                    {metadata: {m: 2}, timestamp: ISODate("2021-05-18T00:00:03.000Z"), a: 9},
                ],
                comment,
            }),
        );
        checkWriteFields({
            db: this.db,
            comment,
            command: "insert",
            expectedNs: tsFullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {ninserted: 1},
        });
    });

    it.skip("TIMESERIES TODO SERVER-134825: timeseries update logs queryShapeHash", function () {
        // Current (incorrect) behavior: query stats for timeseries updates are disabled
        // (see TODO SERVER-119643 in write_ops_exec.cpp), so queryShapeHash is absent from the
        // slow query log.
        const comment = comments.tsUpdate;
        assert.commandWorked(
            this.db.runCommand({
                update: tsCollName,
                updates: [{q: {metadata: {m: 1}}, u: {$set: {b: 1}}, multi: true}],
                comment,
            }),
        );
        checkWriteFields({
            db: this.db,
            comment,
            type: "update",
            expectedNs: tsFullNs,
            expectQueryShapeHash: true,
            expectedMetrics: {nMatched: 3, nModified: 3},
        });
    });

    it.skip("BULKWRITE TODO SERVER-134827: bulkWrite ops log queryShapeHash", function () {
        // Current (incorrect) behavior: bulkWrite op entries do not include a queryShapeHash.
        resetColl(this.db);
        const comment = comments.bulkWrite;
        assert.commandWorked(
            this.db.getSiblingDB("admin").runCommand({
                bulkWrite: 1,
                ops: [{insert: 0, document: {a: 20}}],
                nsInfo: [{ns: fullNs}],
                comment,
            }),
        );
        checkWriteFields({
            db: this.db,
            comment,
            command: "insert",
            expectedNs: fullNs,
            expectQueryShapeHash: true,
        });
    });

    it.skip("BULKWRITE TODO SERVER-134827: bulkWrite command entry reports collectionType of its targets", function () {
        // Current (incorrect) behavior: the top-level bulkWrite slow query log entry has ns
        // "<db>.$cmd" and collectionType "admin", even though the ops target "db.coll". The
        // follow-up should either surface the target collectionType or omit the field.
        resetColl(this.db);
        const comment = comments.bulkWrite;
        assert.commandWorked(
            this.db.getSiblingDB("admin").runCommand({
                bulkWrite: 1,
                ops: [{insert: 0, document: {a: 20}}],
                nsInfo: [{ns: fullNs}],
                comment,
            }),
        );
        checkCollectionType({
            db: this.db,
            comment,
            type: "command",
            expectedCollType: "normal",
        });
    });
});
