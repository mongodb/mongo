/**
 *
 * Tests that runs a shard split to completion and tries to write before and during the split.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');  // for "startParallelShell"
load("jstests/serverless/libs/basic_serverless_test.js");

/*
 * Connects to a replica set and runs write operation, returning the results.
 * @param {rstArgs} replicaSetArgs for the replica set to connect to.
 * @param {tenantIds} perform a write operation for each tenantId.
 */
function doWriteOperations(rstArgs, tenantIds) {
    load("jstests/replsets/rslib.js");

    const donorRst = createRst(rstArgs, true);
    const donorPrimary = donorRst.getPrimary();

    const writeResults = [];

    tenantIds.forEach(id => {
        const kDbName = `${id}_testDb`;
        const kCollName = "testColl";
        const kNs = `${kDbName}.${kCollName}`;

        const res = donorPrimary.getDB(kDbName)
                        .getCollection(kNs)
                        .insert([{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}],
                                {writeConcern: {w: "majority"}})
                        .getRawResponse();
        if (res.writeErrors.length > 0) {
            writeResults.push(res.writeErrors[0].code);
        } else {
            // Push OK
            writeResults.push(0);
        }
    });

    return writeResults;
}
