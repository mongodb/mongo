/**
 * Tests that the transaction items in the 'twoPhaseCommitCoordinator' object in currentOp() are
 * being tracked correctly.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
'use strict';
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function curOpAfterFailpoint(fpName, filter, fpCount, curOpParams) {
    const expectedLog = "Hit " + fpName + " failpoint";
    jsTest.log(`waiting for failpoint '${fpName}' to appear in the log ${fpCount} time(s).`);
    waitForFailpoint(expectedLog, fpCount);

    jsTest.log(`Running curOp operation after '${fpName}' failpoint.`);
    let result = adminDB.aggregate([{$currentOp: curOpParams}, {$match: filter}]).toArray();

    jsTest.log(`${result.length} matching curOp entries after '${fpName}':\n${tojson(result)}`);

    assert.commandWorked(coordinator.adminCommand({
        configureFailPoint: fpName,
        mode: "off",
    }));

    return result;
}

function enableFailPoints(shard, failPoints) {
    failPoints.forEach(function(fpName) {
        assert.commandWorked(shard.adminCommand({
            configureFailPoint: fpName,
            mode: "alwaysOn",
        }));
    });
}

function startTransaction(session, collectionName, insertValue) {
    const dbName = session.getDatabase('test');
    jsTest.log(`Starting a new transaction on ${dbName}.${collectionName}`);
    session.startTransaction();
    // insert into both shards
    assert.commandWorked(dbName[collectionName].insert({_id: -1 * insertValue}));
    assert.commandWorked(dbName[collectionName].insert({_id: insertValue}));

    return [session.getTxnNumber_forTesting(), session.getSessionId()];
}

function commitTxn(st, lsid, txnNumber) {
    let cmd = "db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "})";
    cmd = "assert.commandWorked(" + cmd + ");";
    return startParallelShell(cmd, st.s.port);
}

function coordinatorCuropFilter(session, txnNumber) {
    return {
        'twoPhaseCommitCoordinator.lsid.id': session.getSessionId().id,
        'twoPhaseCommitCoordinator.txnNumber': txnNumber,
        'twoPhaseCommitCoordinator.state': {$exists: true},
    };
}

function undefinedToZero(num) {
    return typeof (num) === 'undefined' ? 0 : num;
}

function assertStepDuration(
    expectedStepDurations, currentDuration, lowerBoundExclusive, stepDurationsDoc) {
    let actualValue = stepDurationsDoc[currentDuration];
    if (expectedStepDurations.includes(currentDuration)) {
        assert.gt(
            actualValue,
            lowerBoundExclusive,
            `expected ${currentDuration} to be > ${lowerBoundExclusive}, got '${actualValue}'`);
    } else {
        assert.eq(typeof (actualValue),
                  'undefined',
                  `expected ${currentDuration} to be undefined, got '${actualValue}'`);
    }
}

function assertCuropFields(coordinator,
                           commitStartCutoff,
                           expectedState,
                           expectedStepDurations,
                           expectedCommitDecision,
                           expectedNumParticipants,
                           result) {
    // mongos broadcasts currentOp to all the shards and puts each shard’s
    // response in a subobject under the shard’s name
    let expectedShardName = coordinator.name.substr(0, coordinator.name.indexOf("/"));
    assert.eq(result.shard, expectedShardName);
    assert.eq("transaction coordinator", result.desc);

    let twoPhaseCommitCoordinatorDoc = result.twoPhaseCommitCoordinator;
    assert.eq(expectedState, twoPhaseCommitCoordinatorDoc.state);
    assert.eq(false, twoPhaseCommitCoordinatorDoc.hasRecoveredFromFailover);
    if (expectedNumParticipants) {
        assert.eq(expectedNumParticipants, twoPhaseCommitCoordinatorDoc.numParticipants);
    }
    if (expectedCommitDecision) {
        assert.eq(twoPhaseCommitCoordinatorDoc.commitDecision.decision, expectedCommitDecision);
    }
    assert.gte(twoPhaseCommitCoordinatorDoc.commitStartTime, commitStartCutoff);
    assert.gt(Date.parse(twoPhaseCommitCoordinatorDoc.deadline), commitStartCutoff);

    let stepDurationsDoc = twoPhaseCommitCoordinatorDoc.stepDurations;
    assertStepDuration(expectedStepDurations, 'writingParticipantListMicros', 0, stepDurationsDoc);
    assertStepDuration(expectedStepDurations, 'waitingForVotesMicros', 0, stepDurationsDoc);
    assertStepDuration(expectedStepDurations, 'writingDecisionMicros', 0, stepDurationsDoc);

    let durationSum = undefinedToZero(stepDurationsDoc.writingParticipantListMicros) +
        undefinedToZero(stepDurationsDoc.waitingForVotesMicros) +
        undefinedToZero(stepDurationsDoc.writingDecisionMicros);

    // make sure totalCommitDuration is at least as big as all the other durations.
    assertStepDuration(
        expectedStepDurations, 'totalCommitDurationMicros', durationSum - 1, stepDurationsDoc);

    let expectedClientFields = ['host', 'client_s', 'connectionId', 'appName', 'clientMetadata'];
    assert.hasFields(result, expectedClientFields);
}

const numShards = 2;
const dbName = "test";
const collectionName = 'currentop_two_phase';
const ns = dbName + "." + collectionName;
const authUser = {
    user: "user",
    pwd: "password",
    roles: jsTest.adminUserRoles
};

function setupCluster(withAuth) {
    let defaultOpts = {rs: {nodes: 1}, shards: numShards, config: 1};
    let authOpts = {other: {keyFile: 'jstests/libs/key1'}};

    let opts = defaultOpts;
    if (withAuth) {
        opts = Object.merge(opts, authOpts);
    }

    const st = new ShardingTest(opts);
    const adminDB = st.s.getDB('admin');
    const coordinator = st.shard0;
    const participant = st.shard1;

    if (withAuth) {
        adminDB.createUser(authUser);
        assert(adminDB.auth(authUser.user, authUser.pwd));
    }

    assert.commandWorked(adminDB.adminCommand({enableSharding: dbName}));
    assert.commandWorked(adminDB.adminCommand({movePrimary: dbName, to: coordinator.shardName}));
    assert.commandWorked(adminDB.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(adminDB.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(
        adminDB.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant.shardName}));
    // this find is to ensure all the shards' filtering metadata are up to date
    assert.commandWorked(st.s.getDB(dbName).runCommand({find: collectionName}));
    return [st, adminDB, coordinator, participant];
}

let [st, adminDB, coordinator, participant] = setupCluster(false);

(function() {
jsTest.log("Check curop coordinator state when idle");
let session = adminDB.getMongo().startSession();
const commitStartCutoff = Date.now();
let [txnNumber, lsid] = startTransaction(session, collectionName, 1);
let expectedState = "inactive";
let filter = coordinatorCuropFilter(session, txnNumber);

let results =
    adminDB.aggregate([{$currentOp: {"idleSessions": false}}, {$match: filter}]).toArray();
jsTest.log(`Curop result(s): ${tojson(results)}`);
assert.eq(0, results.length);

results = adminDB.aggregate([{$currentOp: {"idleSessions": true}}, {$match: filter}]).toArray();
jsTest.log(`Curop result(s): ${tojson(results)}`);
assert.eq(1, results.length);
assertCuropFields(coordinator, commitStartCutoff, expectedState, [], null, 0, results[0]);
})();

(function() {
jsTest.log("Check curop coordinator state while transaction is executing.");
let session = adminDB.getMongo().startSession();
const commitStartCutoff = Date.now();
let [txnNumber, lsid] = startTransaction(session, collectionName, 2);

let failPointStates = {
    'hangBeforeWritingParticipantList': {
        'expectNumFailPoints': 1,
        'expectedState': 'writingParticipantList',
        'expectedStepDurations': ['writingParticipantListMicros', 'totalCommitDurationMicros'],
        'expectedCommitDecision': null,
        'expectedNumParticipants': numShards,
    },
    'hangBeforeSendingPrepare': {
        'expectNumFailPoints': 2,
        'expectedState': 'waitingForVotes',
        'expectedStepDurations':
            ['writingParticipantListMicros', 'waitingForVotesMicros', 'totalCommitDurationMicros'],
        'expectedCommitDecision': null,
        'expectedNumParticipants': numShards,
    },
    'hangBeforeWaitingForDecisionWriteConcern': {
        'expectNumFailPoints': 1,
        'expectedState': 'writingDecision',
        'expectedStepDurations': [
            'writingParticipantListMicros',
            'waitingForVotesMicros',
            'writingDecisionMicros',
            'totalCommitDurationMicros'
        ],
        'expectedCommitDecision': 'commit',
        'expectedNumParticipants': numShards,
    },
    'hangBeforeSendingCommit': {
        'expectNumFailPoints': 2,
        'expectedState': 'waitingForDecisionAck',
        'expectedStepDurations': [
            'writingParticipantListMicros',
            'waitingForVotesMicros',
            'writingDecisionMicros',
            'waitingForDecisionAcksMicros',
            'totalCommitDurationMicros'
        ],
        'expectedCommitDecision': 'commit',
        'expectedNumParticipants': numShards,
    },
    'hangBeforeDeletingCoordinatorDoc': {
        'expectNumFailPoints': 1,
        'expectedState': 'deletingCoordinatorDoc',
        'expectedStepDurations': [
            'writingParticipantListMicros',
            'waitingForVotesMicros',
            'writingDecisionMicros',
            'waitingForDecisionAcksMicros',
            'deletingCoordinatorDocMicros',
            'totalCommitDurationMicros'
        ],
        'expectedCommitDecision': 'commit',
        'expectedNumParticipants': numShards,
    }
};

// Not using 'Object.keys(failPointStates)' since lexical order is not guaranteed
let failPoints = [
    'hangBeforeWritingParticipantList',
    'hangBeforeSendingPrepare',
    'hangBeforeWaitingForDecisionWriteConcern',
    'hangBeforeSendingCommit',
    'hangBeforeDeletingCoordinatorDoc'
];
enableFailPoints(coordinator, failPoints);

let commitJoin = commitTxn(st, lsid, txnNumber);

failPoints.forEach(function(failPoint) {
    let expectNumFailPoints = failPointStates[failPoint].expectNumFailPoints;
    let expectedState = failPointStates[failPoint].expectedState;
    let expectedStepDurations = failPointStates[failPoint].expectedStepDurations;
    let expectedCommitDecision = failPointStates[failPoint].commitDecision;
    let expectedNumParticipants = failPointStates[failPoint].expectedNumParticipants;

    let filter = coordinatorCuropFilter(session, txnNumber, expectedState);
    let results = curOpAfterFailpoint(failPoint, filter, expectNumFailPoints, {idleSessions: true});

    assert.eq(1, results.length);
    assertCuropFields(coordinator,
                      commitStartCutoff,
                      expectedState,
                      expectedStepDurations,
                      expectedCommitDecision,
                      expectedNumParticipants,
                      results[0]);
});

commitJoin();
})();
st.stop();

(function() {
[st, adminDB, coordinator, participant] = setupCluster(true);
jsTest.log("Check curop allUsers flag with auth enabled");
let session = adminDB.getMongo().startSession();
const commitStartCutoff = Date.now();
let [txnNumber, _] = startTransaction(session, collectionName, 1);
let filter = coordinatorCuropFilter(session, txnNumber);

let results = adminDB.aggregate([{$currentOp: {'allUsers': false}}, {$match: filter}]).toArray();
jsTest.log(`Curop result(s): ${tojson(results)}`);
assert.eq(0, results.length);

results = adminDB.aggregate([{$currentOp: {'allUsers': true}}, {$match: filter}]).toArray();
jsTest.log(`Curop result(s): ${tojson(results)}`);
assert.eq(1, results.length);
assertCuropFields(coordinator, commitStartCutoff, 'inactive', [], null, null, results[0]);
})();

st.stop();
})();
