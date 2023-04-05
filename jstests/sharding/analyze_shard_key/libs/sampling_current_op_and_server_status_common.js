/**
 * Defines helpers for testing currentOp and serverStatus for query sampling.
 */

function getCurrentOpAndServerStatusMongos(conn) {
    const currentOp = conn.getDB("admin")
                          .aggregate([
                              {$currentOp: {allUsers: true, localOps: true}},
                              {$match: {desc: "query analyzer"}}
                          ])
                          .toArray();
    const serverStatus = assert.commandWorked(conn.adminCommand({serverStatus: 1})).queryAnalyzers;
    return {currentOp, serverStatus};
}

function getCurrentOpAndServerStatusMongod(conn) {
    const currentOp =
        assert.commandWorked(conn.adminCommand({currentOp: true, desc: "query analyzer"})).inprog;
    const serverStatus = assert.commandWorked(conn.adminCommand({serverStatus: 1})).queryAnalyzers;
    return {currentOp, serverStatus};
}

function validateCurrentOpMongos(currentOp) {
    assert(currentOp.hasOwnProperty("desc"), currentOp);
    assert(currentOp.hasOwnProperty("ns"), currentOp);
    assert(currentOp.hasOwnProperty("collUuid"), currentOp);
    assert(currentOp.hasOwnProperty("sampleRate"), currentOp);
    assert(currentOp.hasOwnProperty("startTime"), currentOp);
    assert(currentOp.hasOwnProperty("sampledReadsCount"), currentOp);
    assert(currentOp.hasOwnProperty("sampledWritesCount"), currentOp);
    assert(!currentOp.hasOwnProperty("sampledReadsBytes"), currentOp);
    assert(!currentOp.hasOwnProperty("sampledWritesBytes"), currentOp);
}

function validateCurrentOpMongod(currentOp, isShardSvr) {
    assert(currentOp.hasOwnProperty("desc"), currentOp);
    assert(currentOp.hasOwnProperty("ns"), currentOp);
    assert(currentOp.hasOwnProperty("collUuid"), currentOp);
    assert.eq(currentOp.hasOwnProperty("sampleRate"), !isShardSvr, currentOp);
    assert(currentOp.hasOwnProperty("startTime"), currentOp);
    assert(currentOp.hasOwnProperty("sampledReadsCount"), currentOp);
    assert(currentOp.hasOwnProperty("sampledWritesCount"), currentOp);
    assert(currentOp.hasOwnProperty("sampledReadsBytes"), currentOp);
    assert(currentOp.hasOwnProperty("sampledWritesBytes"), currentOp);
}

function validateServerStatusMongos(serverStatus) {
    assert(serverStatus.hasOwnProperty("activeCollections"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalCollections"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalSampledReadsCount"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalSampledWritesCount"), serverStatus);
    assert(!serverStatus.hasOwnProperty("totalSampledReadsBytes"), serverStatus);
    assert(!serverStatus.hasOwnProperty("totalSampledWritesBytes"), serverStatus);
}

function validateServerStatusMongod(serverStatus) {
    assert(serverStatus.hasOwnProperty("activeCollections"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalCollections"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalSampledReadsCount"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalSampledWritesCount"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalSampledReadsBytes"), serverStatus);
    assert(serverStatus.hasOwnProperty("totalSampledWritesBytes"), serverStatus);
}

function makeInitialCurrentOpAndServerStatusMongos(numColls) {
    return {
        currentOp: Array(numColls).fill({sampledReadsCount: 0, sampledWritesCount: 0}),
        serverStatus: {
            activeCollections: numColls,
            totalCollections: numColls,
            totalSampledReadsCount: 0,
            totalSampledWritesCount: 0
        }
    };
}

function makeInitialCurrentOpAndServerStatusMongod(numColls) {
    return {
        currentOp: Array(numColls).fill({
            sampledReadsCount: 0,
            sampledWritesCount: 0,
            sampledReadsBytes: 0,
            sampledWritesBytes: 0
        }),
        serverStatus: {
            activeCollections: numColls,
            totalCollections: numColls,
            totalSampledReadsCount: 0,
            totalSampledWritesCount: 0,
            totalSampledReadsBytes: 0,
            totalSampledWritesBytes: 0
        }
    };
}

const opKindRead = 0;
const opKindWrite = 1;
const opKindNoop = 2;

/**
 * Validates the mongos currentOp and serverStatus attached in 'newState' given that the mongos has
 * just executed the operation 'opKind' and its currentOp and serverStatus prior to that are as
 * attached in 'oldState'.
 */
function assertCurrentOpAndServerStatusMongos(
    ns, opKind, oldState, newState, {expectedSampleRate} = {}) {
    const errMsg = {opKind, oldState, newState};

    validateCurrentOpMongos(newState.currentOp[0]);
    assert.eq(newState.currentOp.length, 1, errMsg);
    assert.eq(newState.currentOp[0].ns, ns, errMsg);
    if (expectedSampleRate !== undefined) {
        assert.eq(newState.currentOp[0].sampleRate, expectedSampleRate, errMsg);
    }

    validateServerStatusMongos(newState.serverStatus);
    assert.eq(
        newState.serverStatus.activeCollections, oldState.serverStatus.activeCollections, errMsg);
    assert.eq(
        newState.serverStatus.totalCollections, oldState.serverStatus.totalCollections, errMsg);

    if (opKind === opKindRead) {
        assert.eq(newState.currentOp[0].sampledReadsCount,
                  oldState.currentOp[0].sampledReadsCount + 1,
                  errMsg);
        assert.eq(newState.currentOp[0].sampledWritesCount,
                  oldState.currentOp[0].sampledWritesCount,
                  errMsg);

        assert.eq(newState.serverStatus.totalSampledReadsCount,
                  oldState.serverStatus.totalSampledReadsCount + 1,
                  errMsg);
        assert.eq(newState.serverStatus.totalSampledWritesCount,
                  oldState.serverStatus.totalSampledWritesCount,
                  errMsg);
    } else if (opKind === opKindWrite) {
        assert.eq(newState.currentOp[0].sampledReadsCount,
                  oldState.currentOp[0].sampledReadsCount,
                  errMsg);
        assert.eq(newState.currentOp[0].sampledWritesCount,
                  oldState.currentOp[0].sampledWritesCount + 1,
                  errMsg);

        assert.eq(newState.serverStatus.totalSampledReadsCount,
                  oldState.serverStatus.totalSampledReadsCount,
                  errMsg);
        assert.eq(newState.serverStatus.totalSampledWritesCount,
                  oldState.serverStatus.totalSampledWritesCount + 1,
                  errMsg);
    } else if (opKind === opKindNoop) {
        assert.eq(newState.currentOp[0].sampledReadsCount,
                  oldState.currentOp[0].sampledReadsCount,
                  errMsg);
        assert.eq(newState.currentOp[0].sampledWritesCount,
                  oldState.currentOp[0].sampledWritesCount,
                  errMsg);

        assert.eq(newState.serverStatus.totalSampledReadsCount,
                  oldState.serverStatus.totalSampledReadsCount,
                  errMsg);
        assert.eq(newState.serverStatus.totalSampledWritesCount,
                  oldState.serverStatus.totalSampledWritesCount,
                  errMsg);
    } else {
        throw new Error("Unknown operation kind " + opKind);
    }

    return true;
}

/**
 * Validates the mongod currentOp and serverStatus attached in 'newState' given that the mongod has
 * just executed the operation 'opKind' and its currentOp and serverStatus prior to that are as
 * attached in 'oldState'.
 */
function assertCurrentOpAndServerStatusMongod(ns, opKind, oldState, newState, isShardSvr) {
    const errMsg = {opKind, oldState, newState};

    validateCurrentOpMongod(newState.currentOp[0], isShardSvr);
    assert.eq(newState.currentOp.length, 1, errMsg);
    assert.eq(newState.currentOp[0].ns, ns, errMsg);

    validateServerStatusMongod(newState.serverStatus);
    assert.eq(
        newState.serverStatus.activeCollections, oldState.serverStatus.activeCollections, errMsg);
    assert.eq(
        newState.serverStatus.totalCollections, oldState.serverStatus.totalCollections, errMsg);

    if (opKind == opKindRead) {
        // On a mongod, the counters are incremented asynchronously so they might not be up-to-date
        // after the command returns.
        if (bsonWoCompare(newState.currentOp[0].sampledReadsCount,
                          oldState.currentOp[0].sampledReadsCount) == 0) {
            return false;
        }
        assert.eq(newState.currentOp[0].sampledReadsCount,
                  oldState.currentOp[0].sampledReadsCount + 1,
                  errMsg);
        assert.eq(newState.currentOp[0].sampledWritesCount,
                  oldState.currentOp[0].sampledWritesCount,
                  errMsg);

        assert.eq(newState.serverStatus.totalSampledReadsCount,
                  oldState.serverStatus.totalSampledReadsCount + 1,
                  errMsg);
        assert.eq(newState.serverStatus.totalSampledWritesCount,
                  oldState.serverStatus.totalSampledWritesCount,
                  errMsg);

        // Instead of figuring out the size of the sampled query document, just make sure that the
        // byte counter is greater than before.
        assert.gt(newState.currentOp[0].sampledReadsBytes,
                  oldState.currentOp[0].sampledReadsBytes,
                  errMsg);
        assert.eq(newState.currentOp[0].sampledWritesBytes,
                  oldState.currentOp[0].sampledWritesBytes,
                  errMsg);

        assert.gt(newState.serverStatus.totalSampledReadsBytes,
                  oldState.serverStatus.totalSampledReadsBytes,
                  errMsg);
        assert.eq(newState.serverStatus.totalSampledWritesBytes,
                  oldState.serverStatus.totalSampledWritesBytes,
                  errMsg);
    } else if (opKind == opKindWrite) {
        // On a mongod, the counters are incremented asynchronously so they might not be up-to-date
        // after the command returns.
        if (bsonWoCompare(newState.currentOp[0].sampledWritesCount,
                          oldState.currentOp[0].sampledWritesCount) == 0) {
            return false;
        }
        assert.eq(newState.currentOp[0].sampledReadsCount,
                  oldState.currentOp[0].sampledReadsCount,
                  errMsg);
        assert.eq(newState.currentOp[0].sampledWritesCount,
                  oldState.currentOp[0].sampledWritesCount + 1,
                  errMsg);

        assert.eq(newState.serverStatus.totalSampledReadsCount,
                  oldState.serverStatus.totalSampledReadsCount,
                  errMsg);
        assert.eq(newState.serverStatus.totalSampledWritesCount,
                  oldState.serverStatus.totalSampledWritesCount + 1,
                  errMsg);

        // Instead of figuring out the size of the sampled query document, just make sure that the
        // byte counter is greater than before.
        assert.eq(newState.currentOp[0].sampledReadsBytes,
                  oldState.currentOp[0].sampledReadsBytes,
                  errMsg);
        assert.gt(newState.currentOp[0].sampledWritesBytes,
                  oldState.currentOp[0].sampledWritesBytes,
                  errMsg);

        assert.eq(newState.serverStatus.totalSampledReadsBytes,
                  oldState.serverStatus.totalSampledReadsBytes,
                  errMsg);
        assert.gt(newState.serverStatus.totalSampledWritesBytes,
                  oldState.serverStatus.totalSampledWritesBytes,
                  errMsg);
    } else {
        throw new Error("Unknown operation kind " + opKind);
    }

    return true;
}
