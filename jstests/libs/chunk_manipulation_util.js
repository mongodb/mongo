//
// Utilities for testing chunk manipulation: moveChunk, mergeChunks, etc.
//

import {startParallelOps} from "jstests/libs/test_background_ops.js";

//
// Start a background moveChunk.
// staticMongod:   Server to use for communication, use
//                 "MongoRunner.runMongod({})" to make one.
// mongosURL:      Like 'localhost:27017'.
// findCriteria:   Like { _id: 1 }, passed to moveChunk's "find" option.
// bounds:         Array of two documents that specify the lower and upper
//                 shard key values of a chunk to move. Specify either the
//                 bounds field or the find field but not both.
// ns:             Like 'dbName.collectionName'.
// toShardId:      Like st.shard1.shardName.
//
// Returns a join function; call it to wait for moveChunk to complete.
//

export function moveChunkParallel(staticMongod,
                                  mongosURL,
                                  findCriteria,
                                  bounds,
                                  ns,
                                  toShardId,
                                  expectSuccess = true,
                                  forceJumbo = false) {
    assert((findCriteria || bounds) && !(findCriteria && bounds),
           'Specify either findCriteria or bounds, but not both.');

    function runMoveChunk(
        mongosURL, findCriteria, bounds, ns, toShardId, expectSuccess, forceJumbo) {
        assert(mongosURL && ns && toShardId, 'Missing arguments.');
        assert((findCriteria || bounds) && !(findCriteria && bounds),
               'Specify either findCriteria or bounds, but not both.');

        var mongos = new Mongo(mongosURL), admin = mongos.getDB('admin'), cmd = {moveChunk: ns};

        // TODO SERVER-82068: Remove workaround
        // Ensure that bounds are encoded without extra escape characters, when `MaxKey` or `MinKey`
        // are used. For example, convert {\n\t\t\t\t\"$maxKey\" : 1\n\t\t\t} to {\"$maxKey\" : 1}.
        if (JSON.stringify(tojson(bounds)).includes("maxKey")) {
            bounds[1] = {[Object.keys(bounds[1])[0]]: MaxKey};
        }
        if (JSON.stringify(tojson(bounds)).includes("minKey")) {
            bounds[0] = {[Object.keys(bounds[0])[0]]: MinKey};
        }

        if (findCriteria) {
            cmd.find = findCriteria;
        } else {
            cmd.bounds = bounds;
        }

        cmd.to = toShardId;
        cmd._waitForDelete = true;
        cmd.forceJumbo = forceJumbo;

        jsTest.log.info({cmd});
        var result = admin.runCommand(cmd);
        jsTest.log.info({result});
        if (expectSuccess) {
            assert(result.ok);
        } else {
            assert.commandFailed(result);
        }
    }

    // Return the join function.
    return startParallelOps(
        staticMongod,
        runMoveChunk,
        [mongosURL, findCriteria, bounds, ns, toShardId, expectSuccess, forceJumbo]);
}

// moveChunk starts at step 0 and proceeds to 1 (it has *finished* parsing
// options), 2 (it has reloaded config and installed MigrationSourceManager) and so on.
export var moveChunkStepNames = {
    parsedOptions: 1,
    installedMigrationSourceManager: 2,
    startedMoveChunk: 3,    // called _recvChunkStart on recipient
    reachedSteadyState: 4,  // recipient reports state is "steady"
    chunkDataCommitted: 5,  // called _recvChunkCommit on recipient
    committed: 6
};

export function numberToName(names, stepNumber) {
    for (var name in names) {
        if (names.hasOwnProperty(name) && names[name] == stepNumber) {
            return name;
        }
    }

    assert(false);
}

//
// Configure a failpoint to make moveChunk hang at a step.
//
export function pauseMoveChunkAtStep(shardConnection, stepNumber) {
    configureMoveChunkFailPoint(shardConnection, stepNumber, 'alwaysOn');
}

//
// Allow moveChunk to proceed past a step.
//
export function unpauseMoveChunkAtStep(shardConnection, stepNumber) {
    configureMoveChunkFailPoint(shardConnection, stepNumber, 'off');
}

export function proceedToMoveChunkStep(shardConnection, stepNumber) {
    jsTest.log('moveChunk proceeding from step "' +
               numberToName(moveChunkStepNames, stepNumber - 1) + '" to "' +
               numberToName(moveChunkStepNames, stepNumber) + '".');

    pauseMoveChunkAtStep(shardConnection, stepNumber);
    unpauseMoveChunkAtStep(shardConnection, stepNumber - 1);
    waitForMoveChunkStep(shardConnection, stepNumber);
}

export function configureMoveChunkFailPoint(shardConnection, stepNumber, mode) {
    assert.between(moveChunkStepNames.parsedOptions,
                   stepNumber,
                   moveChunkStepNames.committed,
                   "incorrect stepNumber",
                   true);
    assert.commandWorked(shardConnection.adminCommand(
        {configureFailPoint: 'moveChunkHangAtStep' + stepNumber, mode: mode}));
}

//
// Wait for moveChunk to reach a step (1 through 7). Assumes only one active
// moveChunk running in shardConnection.
//
export function waitForMoveChunkStep(shardConnection, stepNumber) {
    var searchString = 'step ' + stepNumber, admin = shardConnection.getDB('admin');

    assert.between(moveChunkStepNames.parsedOptions,
                   stepNumber,
                   moveChunkStepNames.committed,
                   "incorrect stepNumber",
                   true);

    var msg = ('moveChunk on ' + shardConnection.shardName + ' never reached step "' +
               numberToName(moveChunkStepNames, stepNumber) + '".');

    assert.soon(function() {
        var inProgressStr = '';
        let in_progress = admin.aggregate([{$currentOp: {allUsers: true, idleConnections: true}}]);

        while (in_progress.hasNext()) {
            let op = in_progress.next();
            inProgressStr += tojson(op);

            if (op.desc && op.desc === "MoveChunk") {
                // Note: moveChunk in join mode will not have the "step" message. So keep on
                // looking if searchString is not found.
                if (op.msg && op.msg.startsWith(searchString)) {
                    return true;
                }
            }
        }

        return false;
    }, msg);
}

export var migrateStepNames = {
    deletedPriorDataInRange: 1,
    copiedIndexes: 2,
    rangeDeletionTaskScheduled: 3,
    cloned: 4,
    catchup: 5,  // About to enter steady state.
    steady: 6,
    done: 7
};

//
// Configure a failpoint to make migration thread hang at a step (1 through 5).
//
export function pauseMigrateAtStep(shardConnection, stepNumber) {
    configureMigrateFailPoint(shardConnection, stepNumber, 'alwaysOn');
}

//
// Allow _recvChunkStart to proceed past a step.
//
export function unpauseMigrateAtStep(shardConnection, stepNumber) {
    configureMigrateFailPoint(shardConnection, stepNumber, 'off');
}

export function proceedToMigrateStep(shardConnection, stepNumber) {
    jsTest.log('Migration thread proceeding from step "' +
               numberToName(migrateStepNames, stepNumber - 1) + '" to "' +
               numberToName(migrateStepNames, stepNumber) + '".');

    pauseMigrateAtStep(shardConnection, stepNumber);
    unpauseMigrateAtStep(shardConnection, stepNumber - 1);
    waitForMigrateStep(shardConnection, stepNumber);
}

export function configureMigrateFailPoint(shardConnection, stepNumber, mode) {
    assert.between(migrateStepNames.deletedPriorDataInRange,
                   stepNumber,
                   migrateStepNames.done,
                   "incorrect stepNumber",
                   true);

    var admin = shardConnection.getDB('admin');
    assert.commandWorked(
        admin.runCommand({configureFailPoint: 'migrateThreadHangAtStep' + stepNumber, mode: mode}));
}

//
// Wait for moveChunk to reach a step (1 through 7).
//
export function waitForMigrateStep(shardConnection, stepNumber) {
    var searchString = 'step ' + stepNumber, admin = shardConnection.getDB('admin');

    assert.between(migrateStepNames.deletedPriorDataInRange,
                   stepNumber,
                   migrateStepNames.done,
                   "incorrect stepNumber",
                   true);

    var msg = ('Migrate thread on ' + shardConnection.shardName + ' never reached step "' +
               numberToName(migrateStepNames, stepNumber) + '".');

    assert.soon(function() {
        // verbose = True so we see the migration thread.
        var in_progress = admin.currentOp(true).inprog;
        for (var i = 0; i < in_progress.length; ++i) {
            var op = in_progress[i];
            if (op.desc && op.desc === 'migrateThread') {
                if (op.hasOwnProperty('msg')) {
                    return op.msg.startsWith(searchString);
                } else {
                    return false;
                }
            }
        }

        return false;
    }, msg);
}

//
// Run the given function in the transferMods phase.
//
export function runCommandDuringTransferMods(
    mongos, staticMongod, ns, findCriteria, bounds, fromShard, toShard, cmdFunc) {
    // Turn on the fail point and wait for moveChunk to hit the fail point.
    pauseMoveChunkAtStep(fromShard, moveChunkStepNames.startedMoveChunk);
    let joinMoveChunk =
        moveChunkParallel(staticMongod, mongos.host, findCriteria, bounds, ns, toShard.shardName);
    waitForMoveChunkStep(fromShard, moveChunkStepNames.startedMoveChunk);

    // Run the commands.
    cmdFunc();

    // Turn off the fail point and wait for moveChunk to complete.
    unpauseMoveChunkAtStep(fromShard, moveChunkStepNames.startedMoveChunk);
    joinMoveChunk();
}

export function killRunningMoveChunk(admin) {
    let inProgressOps = admin.aggregate([{$currentOp: {'allUsers': true}}]);
    var abortedMigration = false;
    let inProgressStr = '';
    let opIdsToKill = {};
    while (inProgressOps.hasNext()) {
        let op = inProgressOps.next();
        inProgressStr += tojson(op);

        // For 4.4 binaries and later.
        if (op.desc && op.desc === "MoveChunk") {
            opIdsToKill["MoveChunk"] = op.opid;
        }
    }

    if (opIdsToKill.MoveChunk) {
        admin.killOp(opIdsToKill.MoveChunk);
        abortedMigration = true;
    }

    assert.eq(
        true, abortedMigration, "Failed to abort migration, current running ops: " + inProgressStr);
}

export function migrationsAreAllowed(db, collName) {
    const configDB = db.getSiblingDB("config");
    const nss = `${db.getName()}.${collName}`;
    return configDB.collections.countDocuments({_id: nss, allowMigrations: {$ne: false}}) > 0;
}
