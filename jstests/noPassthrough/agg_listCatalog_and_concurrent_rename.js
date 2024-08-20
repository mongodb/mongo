/**
 * Performs a test that validates concurrent rename operations do not affect the $listCatalog stage.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

// Initialise metadata for the collection we'll test.
const primary = rst.getPrimary();
const testDB = primary.getDB("test");

testDB.createCollection("coll");

const coll = testDB.coll;

assert.commandWorked(coll.createIndex({x: 1}));

function concurrentAggregation(uuid, collName) {
    const res = db.runCommand({
        aggregate: collName,
        collectionUUID: uuid,
        readConcern: {level: "majority"},
        cursor: {},
        pipeline: [{"$listCatalog": {}}]
    });

    // Considering the concurrent rename we expect this aggregation to fail.
    assert.commandFailedWithCode(res,
                                 [ErrorCodes.CollectionUUIDMismatch],
                                 `Expected failure, encountered: ${JSON.stringify(res)}`);
}
const uuid = testDB.getCollectionInfos({name: coll.getName()})[0].info.uuid;

// We now enable the failpoint so that the concurrent aggregation stops right after creating the
// executor. Execution of the aggregation should now either succed with data, or fail.
const fp = configureFailPoint(primary, "hangAfterCreatingAggregationPlan", {uuid: uuid});

const aggregateShell =
    startParallelShell(funWithArgs(concurrentAggregation, uuid, coll.getName()), primary.port);

fp.wait();

// Perform the concurrent rename now. This should cause the concurrent operation to fail.
coll.renameCollection("coll2");

fp.off();

aggregateShell();

rst.stopSet();
