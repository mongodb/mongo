/*
 * TODO SERVER-101609 remove this library once 9.0 becomes lastLTS
 * By then only viewless timeseries will exists so we won't need these functionalities
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {isFCVlt} from "jstests/libs/feature_compatibility_version.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

export function areViewlessTimeseriesEnabled(db) {
    return FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");
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

/**
 * TODO SERVER-101609 once 9.0 becomes last LTS we can remove this function and directly use
 * FixtureHelpers::isSharded on the given collection.
 */
export function isShardedTimeseries(coll) {
    return FixtureHelpers.isSharded(coll) || FixtureHelpers.isSharded(getTimeseriesBucketsColl(coll));
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
    let targetColl = (() => {
        if (
            commandResult.command.findAndModify &&
            FixtureHelpers.isTracked(getTimeseriesCollForDDLOps(db, coll)) &&
            !areViewlessTimeseriesEnabled(db)
        ) {
            // In sharded clusters for findAndModify over legacy tracked timeseries we convert the namespace on the router and we send the command
            // with translated namespace to the shard,
            // thus we expect explain to report the command targeting system.buckets internal namespace.
            return getTimeseriesCollForDDLOps(db, coll);
        }
        return getTimeseriesCollForRawOps(db, coll);
    })();

    if (commandResult.command.findAndModify && isFCVlt(db.getMongo(), "8.3")) {
        // In versions 8.2 findAndModify explain return the main namespace instead of the system.buckets
        // TODO SERVER-114161 enable the check once the fix have been backported to previous versions
        jsTest.log(
            "Skipping namespace check for findAndModify explain output since FCV is less then 8.3 (BACKPORT-26389)",
        );
    } else if (
        commandResult.command.findAndModify &&
        !areViewlessTimeseriesEnabled(db) &&
        (mayConcurrentlyTrackOrUntrack ||
            (TestData.runningWithBalancer &&
                FixtureHelpers.isTracked(getTimeseriesCollForDDLOps(db, coll)) &&
                !FixtureHelpers.isSharded(getTimeseriesCollForDDLOps(db, coll))))
    ) {
        // If the collection is tracked or untracked findAndModify explain returns either the buckets or main timeseries namespace
        // In suites with enabled balancer the collection could randomly became tracked.
        jsTest.log(
            "Skipping namespace check for findAndModify explain output since we don't know if the collection was tracked or not when the command was executed",
        );
    } else {
        jsTest.log(`commandRes = ${tojson(commandResult)}`);
        assert.eq(
            commandResult.command[commandName],
            targetColl.getName(),
            `Expected command namespace to be ${tojson(targetColl.getName())} but got ${tojson(
                commandResult.command[commandName],
            )}`,
        );
    }
}
