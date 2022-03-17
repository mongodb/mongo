/**
 * Util functions used by cluster server parameter tests.
 *
 * When adding new cluster server parameters, do the following:
 * 1. Add its name to clusterParameterNames.
 * 2. Add the clusterParameter document that's expected as default to clusterParametersDefault.
 * 3. Add the clusterParameter document that setClusterParameter is expected to insert after its
 *    first invocation to clusterParametersInsert.
 * 4. Add the clusterParameter document that setClusterParameter is expected to update to after its
 *    second invocation to clusterParametersUpdate.
 *
 */

const clusterParameterNames = ["testStrClusterParameter", "testIntClusterParameter"];
const clusterParametersDefault = [
    {
        _id: "testStrClusterParameter",
        clusterParameterTime: Timestamp(0, 0),
        strData: "off",
    },
    {
        _id: "testIntClusterParameter",
        clusterParameterTime: Timestamp(0, 0),
        intData: 16,
    }
];

const clusterParametersInsert = [
    {
        _id: "testStrClusterParameter",
        clusterParameterTime: Timestamp(10, 2),
        strData: "on",
    },
    {
        _id: "testIntClusterParameter",
        clusterParameterTime: Timestamp(10, 2),
        intData: 17,
    }
];

const clusterParametersUpdate = [
    {
        _id: "testStrClusterParameter",
        clusterParameterTime: Timestamp(20, 4),
        strData: "sleep",
    },
    {
        _id: "testIntClusterParameter",
        clusterParameterTime: Timestamp(20, 4),
        intData: 18,
    }
];

// Set the log level for get/setClusterParameter logging to appear.
function setupNode(conn) {
    const adminDB = conn.getDB('admin');
    adminDB.setLogLevel(2);
}

function setupReplicaSet(rst) {
    setupNode(rst.getPrimary());

    rst.getSecondaries().forEach(function(secondary) {
        setupNode(secondary);
    });
}

function setupSharded(st) {
    setupNode(st.s0);

    const shards = [st.rs0, st.rs1, st.rs2];
    shards.forEach(function(shard) {
        setupReplicaSet(shard);
    });
}

// TO-DO SERVER-65128: replace this function with a call to setClusterParameter.
// Upserts config.clusterParameters document with w:majority.
function simulateSetClusterParameterReplicaSet(rst, query, update) {
    const clusterParametersNS = rst.getPrimary().getDB('config').clusterParameters;
    assert.commandWorked(
        clusterParametersNS.update(query, update, {upsert: true, writeConcern: {w: "majority"}}));
}

// TO-DO SERVER-65128: replace this function with a call to setClusterParameter.
// Upserts config.clusterParameters document with w:majority into configsvr and all shards.
function simulateSetClusterParameterSharded(st, query, update) {
    simulateSetClusterParameterReplicaSet(st.configRS, query, update);

    const shards = [st.rs0, st.rs1, st.rs2];
    shards.forEach(function(shard) {
        simulateSetClusterParameterReplicaSet(shard, query, update);
    });
}

// Runs getClusterParameter on a specific mongod or mongos node and returns true/false depending
// on whether .
function runGetClusterParameterNode(conn, getClusterParameterArgs, expectedClusterParameters) {
    const adminDB = conn.getDB('admin');
    const actualClusterParameters =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: getClusterParameterArgs}))
            .clusterParameters;

    // Sort the returned clusterParameters and the expected clusterParameters by _id.
    actualClusterParameters.sort((a, b) => a._id.localeCompare(b._id));
    expectedClusterParameters.sort((a, b) => a._id.localeCompare(b._id));
    for (let i = 0; i < expectedClusterParameters.length; i++) {
        const expectedClusterParameter = expectedClusterParameters[i];
        const actualClusterParameter = actualClusterParameters[i];

        // Sort both expectedClusterParameter and actualClusterParameter into alphabetical order
        // by key.
        const sortedExpectedClusterParameter =
            Object.keys(expectedClusterParameter).sort().reduce(function(sorted, key) {
                sorted[key] = expectedClusterParameter[key];
                return sorted;
            }, {});
        const sortedActualClusterParameter =
            Object.keys(actualClusterParameter).sort().reduce(function(sorted, key) {
                sorted[key] = actualClusterParameter[key];
                return sorted;
            }, {});
        if (bsonWoCompare(sortedExpectedClusterParameter, sortedActualClusterParameter) !== 0) {
            return false;
        }
    }

    return true;
}

// Runs getClusterParameter on each replica set node and asserts that the response matches the
// expected parameter objects on at least a majority of nodes.
function runGetClusterParameterReplicaSet(rst, getClusterParameterArgs, expectedClusterParameters) {
    let numMatches = 0;
    const numTotalNodes = rst.getSecondaries().length + 1;
    if (runGetClusterParameterNode(
            rst.getPrimary(), getClusterParameterArgs, expectedClusterParameters)) {
        numMatches++;
    }

    rst.getSecondaries().forEach(function(secondary) {
        if (runGetClusterParameterNode(
                secondary, getClusterParameterArgs, expectedClusterParameters)) {
            numMatches++;
        }
    });

    assert((numMatches / numTotalNodes) > 0.5);
}

// Runs getClusterParameter on mongos and each mongod in each shard replica set.
function runGetClusterParameterSharded(st, getClusterParameterArgs, expectedClusterParameters) {
    runGetClusterParameterNode(st.s0, getClusterParameterArgs, expectedClusterParameters);

    const shards = [st.rs0, st.rs1, st.rs2];
    shards.forEach(function(shard) {
        runGetClusterParameterReplicaSet(shard, getClusterParameterArgs, expectedClusterParameters);
    });
}

// Tests valid usages of getClusterParameter and verifies that the expected values are returned.
function testValidParameters(conn) {
    if (conn instanceof ReplSetTest) {
        // Run getClusterParameter in list format and '*' and ensure it returns all default values
        // on all nodes in the replica set.
        runGetClusterParameterReplicaSet(conn, clusterParameterNames, clusterParametersDefault);
        runGetClusterParameterReplicaSet(conn, '*', clusterParametersDefault);

        // For each parameter, simulate setClusterParameter and verify that getClusterParameter
        // returns the updated value on all nodes in the replica set.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            query = {_id: clusterParameterNames[i]};
            simulateSetClusterParameterReplicaSet(conn, query, clusterParametersInsert[i]);
            runGetClusterParameterReplicaSet(
                conn, clusterParameterNames[i], [clusterParametersInsert[i]]);
        }

        // Do the above again to verify that document updates are also handled properly.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            query = {_id: clusterParameterNames[i]};
            simulateSetClusterParameterReplicaSet(conn, query, clusterParametersUpdate[i]);
            runGetClusterParameterReplicaSet(
                conn, clusterParameterNames[i], [clusterParametersUpdate[i]]);
        }

        // Finally, run getClusterParameter in list format and '*' and ensure that they now all
        // return updated values.
        runGetClusterParameterReplicaSet(conn, clusterParameterNames, clusterParametersUpdate);
        runGetClusterParameterReplicaSet(conn, '*', clusterParametersUpdate);
    } else {
        // Run getClusterParameter in list format and '*' and ensure it returns all default values
        // on all nodes in the sharded cluster.
        runGetClusterParameterSharded(conn, clusterParameterNames, clusterParametersDefault);
        runGetClusterParameterSharded(conn, '*', clusterParametersDefault);

        // For each parameter, simulate setClusterParameter and verify that getClusterParameter
        // returns the updated value on all nodes in the sharded cluster.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            query = {_id: clusterParameterNames[i]};
            simulateSetClusterParameterSharded(conn, query, clusterParametersInsert[i]);
            runGetClusterParameterSharded(
                conn, clusterParameterNames[i], [clusterParametersInsert[i]]);
        }

        // Do the above again to verify that document updates are also handled properly.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            query = {_id: clusterParameterNames[i]};
            simulateSetClusterParameterSharded(conn, query, clusterParametersUpdate[i]);
            runGetClusterParameterSharded(
                conn, clusterParameterNames[i], [clusterParametersUpdate[i]]);
        }

        // Finally, run getClusterParameter in list format and '*' and ensure that they now all
        // return updated values.
        runGetClusterParameterSharded(conn, clusterParameterNames, clusterParametersUpdate);
        runGetClusterParameterSharded(conn, '*', clusterParametersUpdate);
    }
}

// Tests that invalid uses of getClusterParameter fails
function testInvalidParametersNode(conn) {
    const adminDB = conn.getDB('admin');
    // Assert that specifying a nonexistent parameter returns an error.
    assert.commandFailedWithCode(adminDB.runCommand({getClusterParameter: "nonexistentParam"}),
                                 ErrorCodes.NoSuchKey);
    assert.commandFailedWithCode(adminDB.runCommand({getClusterParameter: ["nonexistentParam"]}),
                                 ErrorCodes.NoSuchKey);
    assert.commandFailedWithCode(
        adminDB.runCommand({getClusterParameter: ["testIntClusterParameter", "nonexistentParam"]}),
        ErrorCodes.NoSuchKey);
}

// Tests that invalid uses of getClusterParameter fail with the appropriate errors.
function testInvalidParameters(conn) {
    if (conn instanceof ReplSetTest) {
        testInvalidParametersNode(conn.getPrimary());
        conn.getSecondaries().forEach(function(secondary) {
            testInvalidParametersNode(secondary);
        });
    } else {
        testInvalidParametersNode(conn.s0);
        const shards = [conn.rs0, conn.rs1, conn.rs2];
        shards.forEach(function(shard) {
            shard.getSecondaries().forEach(function(secondary) {
                testInvalidParametersNode(secondary);
            });
        });
    }
}
