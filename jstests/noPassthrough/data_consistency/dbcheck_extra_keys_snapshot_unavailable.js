/**
 * Tests that the dbCheck extra index keys check tolerates SnapshotUnavailable when the collection
 * under check is dropped and its data is no longer available at the batch's read timestamp.
 *
 * @tags: [requires_replication]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertForDbCheckErrors,
    clearHealthLog,
    runDbCheck,
} from "jstests/replsets/libs/dbcheck_utils.js";

describe("dbCheck extra index keys check", () => {
    let rst;
    let primary;
    let db;

    before(() => {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}},
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        db = primary.getDB("test");
    });

    after(() => {
        rst.stopSet();
    });

    it("can retry dbCheck for SnapshotUnavailable without logging an error", () => {
        const coll = db.coll;
        const holeColl = db.hole;
        assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
        assert.commandWorked(db.createCollection(holeColl.getName()));
        clearHealthLog(rst);

        const hashingFp = configureFailPoint(primary, "hangBeforeExtraIndexKeysHashing");
        runDbCheck(rst, db, coll.getName(), {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "_id_",
        });
        hashingFp.wait();

        // Hold an oplog hole open so the batch's no-overlap read timestamp stays before the drop.
        const holeFp = configureFailPoint(primary, "hangAfterCollectionInserts", {
            collectionNS: holeColl.getFullName(),
        });
        const insertThread = new Thread(
            (host, ns) => {
                const [dbName, collName] = ns.split(/\.(.*)/);
                return new Mongo(host).getDB(dbName)[collName].insert({_id: 0});
            },
            primary.host,
            holeColl.getFullName(),
        );
        insertThread.start();
        holeFp.wait();

        assert.commandWorked(db.runCommand({drop: coll.getName(), writeConcern: {w: 1}}));

        const pitFp = configureFailPoint(primary, "failToCreatePITCollectionFromDropPendingIdent");
        hashingFp.off();

        const healthlog = primary.getDB("local").system.healthlog;
        assert.soon(
            () => healthlog.find({operation: "dbCheckStop"}).itcount() == 1,
            "dbCheck did not stop",
        );

        pitFp.off();
        holeFp.off();
        insertThread.join();
        assert.commandWorked(insertThread.returnData());

        assert.eq(
            healthlog.find({severity: "error", "data.error": /^SnapshotUnavailable/}).itcount(),
            0,
            "dbCheck logged SnapshotUnavailable as an error",
            {healthlog: healthlog.find().toArray()},
        );
        assertForDbCheckErrors(primary, true /* assertForErrors */);
    });
});
