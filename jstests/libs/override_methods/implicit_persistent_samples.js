/**
 * Override for passthrough testing of CBR with persistent samples:
 *   1. After every write that adds or removes at least one document, runs `analyze` in sample mode
 *      to keep the persisted sample current. Clears tracking on drop/dropDatabase.
 *   2. For every query command, runs the original command and only if it succeeds, runs explain
 *      and checks that ceSamplingMetadata reports a persisted sample for the command's target namespace.
 *   3. On every explicit explain result that includes ceSamplingMetadata, performs the same check.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kQueryCommands = new Set([
    "find",
    "aggregate",
    "count",
    "distinct",
    "findAndModify",
    "findandmodify",
]);
// System DBs (admin/local/config) are populated by internals, not user inserts, so we should skip
// analysis and metadata checks.
const kSystemDbs = new Set(["admin", "local", "config"]);

// "dbName.collName" entries successfully analyzed this session; used to distinguish analyzed
// (need persisted sample) from unanalyzed (must be empty or a view).
const analyzedNamespaces = new Set();

function analyzeNamespace(conn, fullNs) {
    const dot = fullNs.indexOf(".");
    OverrideHelpers.withPreOverrideRunCommand(() =>
        assert.commandWorked(
            conn.getDB(fullNs.slice(0, dot)).runCommand({
                analyze: fullNs.slice(dot + 1),
                mode: "sample",
                samplingMethod: "random",
            }),
        ),
    );
    analyzedNamespaces.add(fullNs);
}

function checkSamplingMetadata(conn, dbName, explainResult, ns) {
    // If we haven't analyzed the namespace then it must either be empty (we do not run analyze for
    // createCollection commands in the override below) or it must be a view (you can't run analyze
    // on the view namespace since the view isn't materialized)
    if (!analyzedNamespaces.has(ns)) {
        const collName = ns.slice(dbName.length + 1);
        const db = conn.getDB(dbName);

        const listCollResult = OverrideHelpers.withPreOverrideRunCommand(() =>
            db.runCommand({listCollections: 1, filter: {name: collName, type: "view"}}),
        );
        assert.commandWorked(listCollResult);
        if (listCollResult.cursor.firstBatch.length > 0) {
            return;
        }

        const countResult = OverrideHelpers.withPreOverrideRunCommand(() =>
            db.runCommand({count: collName}),
        );
        assert.commandWorked(countResult);
        assert.eq(0, countResult.n, "expected unanalyzed namespace to be empty", {ns});
        return;
    }
    const samplingMetadata = explainResult.queryPlanner?.ceSamplingMetadata;
    if (!samplingMetadata || !samplingMetadata[ns]) {
        // No sampling metadata for this namespace: either CBR didn't use sampling CE at all, or
        // it did but didn't need to estimate cardinality for this namespace (e.g. a query with no
        // filter has no selectivity to estimate). Either way, the winning plan must not claim
        // "Sampling" as its CE source — that would mean sampling was used but left no record.
        const ceSource = explainResult.queryPlanner?.winningPlan?.estimatesMetadata?.ceSource;
        assert.neq(
            "Sampling",
            ceSource,
            "expected no Sampling ceSource when ceSamplingMetadata entry is absent",
            {ns, ceSource, samplingMetadata},
        );
        return;
    }
    assert.eq(
        "persisted",
        samplingMetadata[ns].sampleSource,
        "expected persistent sample in ceSamplingMetadata",
        {ns, meta: samplingMetadata[ns]},
    );
}

export function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    const result = clientFunction.apply(conn, makeFuncArgs(cmdObj));

    // Commands with fromRouter:true are sent on internalClient connections, which require explicit
    // writeConcern on every write. Skip all injected commands for these.
    if (cmdObj.fromRouter || !result.ok) {
        return result;
    }

    const isFindAndModify = cmdName.toLowerCase() === "findandmodify";
    const needsAnalyze =
        (cmdName === "insert" && result.n > 0) ||
        (cmdName === "update" && (result.upserted ?? []).length > 0) ||
        (cmdName === "delete" && result.n > 0) ||
        (isFindAndModify && result.lastErrorObject?.upserted !== undefined) ||
        (isFindAndModify && cmdObj.remove && (result.lastErrorObject?.n ?? 0) > 0);
    if (needsAnalyze) {
        analyzeNamespace(conn, `${dbName}.${cmdObj[cmdName]}`);
    }

    if (cmdName === "drop") {
        analyzedNamespaces.delete(`${dbName}.${cmdObj.drop}`);
    } else if (cmdName === "dropDatabase") {
        const prefix = `${dbName}.`;
        for (const ns of analyzedNamespaces) {
            if (ns.startsWith(prefix)) {
                analyzedNamespaces.delete(ns);
            }
        }
    }

    if (
        cmdName === "bulkWrite" &&
        (result.nInserted > 0 || result.nDeleted > 0 || result.nUpserted > 0)
    ) {
        // Iterate result cursor (not cmdObj.ops) to skip unexecuted ops (e.g., ordered:true halted
        // on earlier error); only analyze namespaces with actual writes.
        const seen = new Set();
        for (const entry of result.cursor?.firstBatch || []) {
            if (!entry.ok || (!entry.n && !entry.upserted)) {
                continue;
            }
            const op = (cmdObj.ops || [])[entry.idx];
            const nsInfoIdx = op?.insert ?? op?.update ?? op?.delete;
            const fullNs = (cmdObj.nsInfo || [])[nsInfoIdx]?.ns;
            if (!fullNs || seen.has(fullNs)) {
                continue;
            }
            seen.add(fullNs);
            analyzeNamespace(conn, fullNs);
        }
    }

    // Run AFTER analyze so findAndModify upserts are tracked first.
    if (
        kQueryCommands.has(cmdName) &&
        typeof cmdObj[cmdName] === "string" && // skips db-level aggregates ({aggregate:1})
        cmdObj.explain == null &&
        !cmdObj.writeConcern && // explain does not support writeConcern
        !kSystemDbs.has(dbName)
    ) {
        const explainResult = OverrideHelpers.withPreOverrideRunCommand(() =>
            conn.getDB(dbName).runCommand({explain: cmdObj, verbosity: "queryPlanner"}),
        );
        assert.commandWorked(explainResult);
        checkSamplingMetadata(conn, dbName, explainResult, `${dbName}.${cmdObj[cmdName]}`);
    }

    if (cmdName === "explain" && !kSystemDbs.has(dbName)) {
        const innerCmd = cmdObj.explain;
        const innerCmdName = [...kQueryCommands].find((name) => innerCmd[name]);
        if (innerCmdName && typeof innerCmd[innerCmdName] === "string") {
            checkSamplingMetadata(conn, dbName, result, `${dbName}.${innerCmd[innerCmdName]}`);
        }
    }

    return result;
}

OverrideHelpers.overrideRunCommand(runCommandOverride);
