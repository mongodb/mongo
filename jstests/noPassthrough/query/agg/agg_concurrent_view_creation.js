/**
 * Performs a test that validates a concurrent view creation doesn't cause issues with the catalog
 * that an aggregation uses.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
testDB.setProfilingLevel(2, {slowms: -1});

const viewName = "viewName";

function concurrentAggregation(viewName) {
    const res = db.runCommand({aggregate: viewName, pipeline: [], cursor: {}});

    assert.commandWorked(res);
}

// We now enable the failpoint so that the concurrent aggregation stops right after acquiring the
// catalog.
const fp =
    configureFailPoint(primary, "hangAfterAcquiringCollectionCatalog", {collection: viewName});

const aggregateShell =
    startParallelShell(funWithArgs(concurrentAggregation, viewName), primary.port);

fp.wait();

// Perform the concurrent DDL operation now. This should not cause the operation to fail.
testDB.createView(viewName, "srcColl", []);

fp.off();

aggregateShell();

rst.stopSet();
