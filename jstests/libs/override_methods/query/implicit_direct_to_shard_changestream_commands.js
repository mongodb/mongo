/**
 * Redirects $changeStream aggregate/getMore/killCursors to shard0's primary directly; every other
 * command still flows through mongos. Mirrors a client (e.g. Atlas Search's mongot) that reads a
 * normally-routed collection's change stream directly off the shard.
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {retryDirectShardCommand} from "jstests/libs/override_methods/query/direct_to_shard_retry_util.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {getChangeStreamStage} from "jstests/libs/query/change_stream_util.js";

// Discovered eagerly: doing this lazily inside the override below would recurse, since topology
// discovery itself runs a command through the same runCommand we're about to patch.
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
const shardName = Object.keys(topology.shards)[0];
const shardConn = new Mongo(topology.shards[shardName].primary);

// Lets tests detect that change-stream commands are redirected.
TestData.changeStreamCommandsDirectToShard = true;

// Captured before installing our override so shard-direct dispatch doesn't recurse back through
// it (Mongo.prototype.runCommand is shared across all Mongo instances, including shardConn).
const mongoRunCommandBeforeOverride = Mongo.prototype.runCommand;

// Retries on StaleConfig and other errors mongos would otherwise absorb transparently.
function runCommandDirectlyOnShard(cmdName, dbName, cmdObj) {
    return retryDirectShardCommand(cmdName, () =>
        mongoRunCommandBeforeOverride.call(shardConn, dbName, cmdObj, 0),
    );
}

const isChangeStreamAggregate = function (cmdObj) {
    return getChangeStreamStage(cmdObj) !== undefined;
};

// Cursor ids opened directly on the shard, so their getMore/killCursors also redirect there.
const directCursorIds = new Set();

function runCommandDirectlyOnShardForChangeStreams(
    conn,
    dbName,
    cmdName,
    cmdObj,
    func,
    makeFuncArgs,
) {
    if (typeof cmdObj !== "object" || cmdObj === null || !conn.isMongos()) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    if (isChangeStreamAggregate(cmdObj)) {
        const res = runCommandDirectlyOnShard("aggregate", dbName, cmdObj);
        if (res.ok && res.cursor && res.cursor.id && String(res.cursor.id) !== "0") {
            directCursorIds.add(String(res.cursor.id));
        }
        return res;
    }

    if (cmdName === "getMore" && directCursorIds.has(String(cmdObj.getMore))) {
        const res = runCommandDirectlyOnShard("getMore", dbName, cmdObj);
        if (!res.ok || !res.cursor || !res.cursor.id || String(res.cursor.id) === "0") {
            directCursorIds.delete(String(cmdObj.getMore));
        }
        return res;
    }

    if (
        cmdName === "killCursors" &&
        Array.isArray(cmdObj.cursors) &&
        cmdObj.cursors.some((id) => directCursorIds.has(String(id)))
    ) {
        const [direct, rest] = [[], []];
        for (const id of cmdObj.cursors) {
            (directCursorIds.has(String(id)) ? direct : rest).push(id);
        }

        const directRes = runCommandDirectlyOnShard("killCursors", dbName, {
            ...cmdObj,
            cursors: direct,
        });
        direct.forEach((id) => directCursorIds.delete(String(id)));
        if (rest.length === 0) {
            return directRes;
        }

        const restRes = func.apply(conn, makeFuncArgs({...cmdObj, cursors: rest}));
        return {
            ...restRes,
            cursorsKilled: [...restRes.cursorsKilled, ...directRes.cursorsKilled],
            cursorsNotFound: [...restRes.cursorsNotFound, ...directRes.cursorsNotFound],
            cursorsAlive: [...restRes.cursorsAlive, ...directRes.cursorsAlive],
            cursorsUnknown: [...restRes.cursorsUnknown, ...directRes.cursorsUnknown],
        };
    }

    return func.apply(conn, makeFuncArgs(cmdObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_direct_to_shard_changestream_commands.js",
);
OverrideHelpers.overrideRunCommand(runCommandDirectlyOnShardForChangeStreams);
