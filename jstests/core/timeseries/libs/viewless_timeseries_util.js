/*
 * TODO SERVER-101609 remove this library once 9.0 becomes lastLTS
 * By then only viewless timeseries will exists so we won't need these functionalities
 */

import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {isFCVlt, isStableFCVSuite} from "jstests/libs/feature_compatibility_version.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Checks if the viewless timeseries feature flag is currently enabled.
// Do not use this function in passthrough tests, because the feature flag may get enabled or
// disabled at any time by the background FCV upgrade/downgrade hook.
export function areViewlessTimeseriesEnabled(db) {
    if (runningWithViewlessTimeseriesUpgradeDowngrade(db)) {
        throw new Error(
            "Checking the viewless timeseries feature flag is racy in a suite where timeseries collections are being converted between viewless (new) and viewful (legacy). " +
                "Use isViewlessTimeseriesOnlySuite, isViewfulTimeseriesOnlySuite or runningWithViewlessTimeseriesUpgradeDowngrade to decide if the test can run and what can be asserted.",
        );
    }

    return FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");
}

// Returns true if the suite converts timeseries across viewful/viewless format in the background.
export function runningWithViewlessTimeseriesUpgradeDowngrade(db) {
    if (isStableFCVSuite()) {
        return false;
    }

    const flagDoc = FeatureFlagUtil.getFeatureFlagDoc(db.getMongo(), "CreateViewlessTimeseriesCollections");
    if (!flagDoc.value) {
        // The feature flag is disabled (in all FCVs).
        return false;
    }

    // Until lastLTSFCV >= flagDoc.version, the flag is disabled on lastLTSFCV and enabled on latestFCV.
    return MongoRunner.compareBinVersions(lastLTSFCV, flagDoc.version) < 0;
}

// Returns true if the suite creates all timeseries collections in viewless format.
export function isViewlessTimeseriesOnlySuite(db) {
    // If we are in a stable FCV suite, then we can just check the current value of the feature flag.
    if (isStableFCVSuite()) {
        return FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");
    }

    // In FCV upgrade/downgrade suite, the suite is viewless-only if the flag has been enabled since
    // last LTS (example: viewless timeseries released in FCV 9.0, we are in binary 9.1 running a
    // FCV 9.0 - FCV 9.1 upgrade/downgrade suite --> all collections are viewless).
    const flagDoc = FeatureFlagUtil.getFeatureFlagDoc(db.getMongo(), "CreateViewlessTimeseriesCollections");
    return flagDoc.value && MongoRunner.compareBinVersions(lastLTSFCV, flagDoc.version) >= 0;
}

// Returns true if the suite creates all timeseries collections in viewful format.
export function isViewfulTimeseriesOnlySuite(db) {
    if (isStableFCVSuite()) {
        return !FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");
    }

    // Check if the viewless timeseries feature flag is disabled in the latest FCV.
    return !FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections", true /* ignoreFCV */);
}

/**
 * Asserts that `value` is truthy for viewless timeseries and falsy for viewful timeseries.
 * In suites with mixed viewless/viewful timeseries, no assertion is made.
 */
export function assertOnlyForViewlessTimeseries(db, value, msg) {
    if (isViewlessTimeseriesOnlySuite(db)) {
        assert(value, msg);
    } else if (isViewfulTimeseriesOnlySuite(db)) {
        assert(!value, msg);
    }
}

/**
 * Given a collection return its corresponding buckets collection.
 *
 * - If the input 'coll' is a DBCollection object (representing the time-series collection),
 * this function returns a DBCollection object for the corresponding system.buckets.*
 * collection residing in the same database.
 * - If the input 'coll' is a string (the name of the time-series collection),
 * this function returns the corresponding system.buckets.* collection name as a string.
 *
 * TODO SERVER-101609 remove this function once 9.0 becomes lastLTS.
 */
export function getTimeseriesBucketsColl(coll) {
    const kBucketsPrefix = "system.buckets.";

    if (typeof coll === "string") {
        // It's a collection name string
        if (coll.trim() === "") {
            throw new Error("Input collection name string cannot be empty.");
        }
        if (coll.startsWith(kBucketsPrefix)) {
            return coll;
        }
        return kBucketsPrefix + coll;
    }
    if (coll instanceof DBCollection) {
        const bucketsName = getTimeseriesBucketsColl(coll.getName());
        return coll.getDB().getCollection(bucketsName);
    }

    // Handle invalid input types
    throw new Error(
        `Invalid parameter. 'coll' must be a collection (DBCollection) or the collection name (string). Receinved parameter '${tojson(
            coll,
        )}' (${typeof coll})`,
    );
}

export function getTimeseriesCollForDDLOps(db, coll) {
    if (areViewlessTimeseriesEnabled(db)) {
        return coll;
    }
    return getTimeseriesBucketsColl(coll);
}

// Runs a chunk command (split, moveChunk, moveRange, etc.) on a timeseries collection, targeting
// the right namespace ("ts" for viewless timeseries, "system.buckets.ts" for viewful timeseries).
// During FCV transitions, it attempts the operation on both namespaces until it works.
// TODO SERVER-101609 once 9.0 becomes last LTS replace this with `db.adminCommand(cmdObj)`.
export function runTimeseriesChunkCommand(db, cmdObj) {
    const cmdName = getCommandName(cmdObj);
    const originalNs = cmdObj[cmdName];
    const coll = db.getMongo().getCollection(originalNs);

    // If the FCV is stable, we don't need to go through the retry loop.
    if (isStableFCVSuite()) {
        const resolvedNs = getTimeseriesCollForDDLOps(db, coll).getFullName();
        return assert.commandWorked(db.adminCommand({...cmdObj, [cmdName]: resolvedNs}));
    }

    const bucketsNs = getTimeseriesBucketsColl(coll).getFullName();

    const expectedErrorCodes = [
        // The wrong namespace was targeted (specific error depends on the command & the point at which it fails).
        ErrorCodes.NamespaceNotFound,
        ErrorCodes.NamespaceNotSharded,
        ErrorCodes.CommandNotSupportedOnView,
        ErrorCodes.StaleConfig,
        // Failed because migrations are blocked during viewless timeseries upgrade/downgrade.
        ErrorCodes.ConflictingOperationInProgress,
    ];

    let lastRes;
    assert.soon(
        () => {
            jsTest.log.info(`Trying timeseries chunk operation ${cmdName} on ${originalNs}`);
            lastRes = db.adminCommand(cmdObj);
            if (lastRes.ok || !expectedErrorCodes.includes(lastRes.code)) {
                return true;
            }

            jsTest.log.info(`Trying timeseries chunk operation ${cmdName} on ${bucketsNs}`);
            lastRes = db.adminCommand({...cmdObj, [cmdName]: bucketsNs});
            if (
                lastRes.ok ||
                (!expectedErrorCodes.includes(lastRes.code) &&
                    lastRes.code !== ErrorCodes.CommandNotSupportedOnLegacyTimeseriesBucketsNamespace)
            ) {
                return true;
            }

            jsTest.log.info(`Backing off because timeseries chunk operation ${cmdName} failed on both namespaces`);
            return false;
        },
        () => `Chunk command failed for ${originalNs}: ${tojson(lastRes)}`,
    );
    assert.commandWorked(lastRes);
    return lastRes;
}

export function findTimeseriesConfigCollectionsDocument(coll) {
    // We must use snapshot read concern to avoid racing with viewless timeseries upgrade/downgrade,
    // so bypass overrides, which may want to impose a different read concern (e.g. majority).
    return OverrideHelpers.withPreOverrideRunCommand(() => {
        try {
            const collEntry = coll
                .getDB()
                .getSiblingDB("config")
                .collections.findOne(
                    {_id: {$in: [coll.getFullName(), getTimeseriesBucketsColl(coll).getFullName()]}},
                    {} /* projection */,
                    {} /* options */,
                    "snapshot",
                );
            return collEntry;
        } catch (e) {
            // readConcern "snapshot" is not supported on standalone nodes, but on a sharded cluster
            // there can be no standalone nodes, so the collection is not sharded.
            if (e.code === ErrorCodes.NotAReplicaSet) {
                return null;
            }
            throw e;
        }
    });
}

/**
 * TODO SERVER-101609 once 9.0 becomes last LTS we can remove this function and directly use
 * FixtureHelpers::isSharded on the given collection.
 */
export function isShardedTimeseries(coll) {
    const collEntry = findTimeseriesConfigCollectionsDocument(coll);
    if (collEntry === null) {
        return false;
    }
    return collEntry.unsplittable === null || !collEntry.unsplittable;
}

/**
 * TODO SERVER-101609 once 9.0 becomes last LTS we can remove this function and directly use
 * FixtureHelpers::isTracked on the given collection.
 */
export function isTrackedTimeseries(coll) {
    return findTimeseriesConfigCollectionsDocument(coll) !== null;
}

/**
 * TODO SERVER-101609 once 9.0 becomes last LTS we can remove this function and directly use
 * FixtureHelpers::numberOfShardsForCollection on the given collection.
 */
export function numberOfShardsForTimeseriesCollection(coll) {
    const collEntry = findTimeseriesConfigCollectionsDocument(coll);
    if (collEntry === null) {
        return 1;
    }
    return coll.getDB().getSiblingDB("config").chunks.distinct("shard", {uuid: collEntry.uuid}).length;
}

/**
 * Checks that the namespace targeted by `commandResult` the command matches `coll`,
 * modulo quirks of translation to system.buckets for legacy timeseries.
 */
export function assertExplainTargetsExpectedTimeseriesNamespace(
    db,
    coll,
    commandResult,
    commandName,
    {mayConcurrentlyTrackOrUntrack = false} = {},
) {
    let targetColl = getTimeseriesCollForRawOps(db, coll);

    if (commandResult.command.findAndModify && !isViewlessTimeseriesOnlySuite(db)) {
        if (
            mayConcurrentlyTrackOrUntrack ||
            (TestData.runningWithBalancer && isTrackedTimeseries(coll) && !isShardedTimeseries(coll))
        ) {
            // If the collection is tracked or untracked findAndModify explain returns either the buckets or main timeseries namespace
            // In suites with enabled balancer the collection could randomly became tracked.
            jsTest.log(
                "Skipping namespace check for findAndModify explain output since we don't know if the collection was tracked or not when the command was executed",
            );
            return;
        }

        if (isTrackedTimeseries(coll)) {
            if (isFCVlt(db.getMongo(), "8.3")) {
                // In versions 8.2 findAndModify explain return the main namespace instead of the system.buckets
                // TODO SERVER-114161 enable the check once the fix have been backported to previous versions
                jsTest.log(
                    "Skipping namespace check for findAndModify explain output since FCV is less then 8.3 (BACKPORT-26389)",
                );
                return;
            }

            // In sharded clusters for findAndModify over legacy tracked timeseries we convert the namespace on the router and we send the command
            // with translated namespace to the shard,
            // thus we expect explain to report the command targeting system.buckets internal namespace.
            if (runningWithViewlessTimeseriesUpgradeDowngrade(db)) {
                jsTest.log(
                    "Skipping namespace check for findAndModify explain output since we don't know if the collection was viewful or not when the command was executed",
                );
                return;
            }

            targetColl = getTimeseriesBucketsColl(coll);
        }
    }

    jsTest.log(`commandRes = ${tojson(commandResult)}`);
    assert.eq(
        commandResult.command[commandName],
        targetColl.getName(),
        `Expected command namespace to be ${tojson(targetColl.getName())} but got ${tojson(
            commandResult.command[commandName],
        )}`,
    );
}
