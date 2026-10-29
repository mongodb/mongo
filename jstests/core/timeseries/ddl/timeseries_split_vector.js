/**
 * Verifies that the internal `splitVector` and `autoSplitVector` commands are rejected on
 * time-series collections instead of silently returning split points (or failing inconsistently).
 *
 * @tags: [
 *   # The commands under test are not allowed with a signed security token.
 *   not_allowed_with_signed_security_token,
 *   # splitVector / autoSplitVector require cluster-level privileges.
 *   assumes_superuser_permissions,
 *   requires_timeseries,
 * ]
 */
import {isViewlessTimeseriesOnlySuite} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const timeField = "time";
const metaField = "meta";
const collName = jsTestName();
const coll = db.getCollection(collName);

// The key patterns a caller might plausibly try to split on: the always-present _id index, plus the
// meta and time fields of the time-series collection.
const keyPatterns = [{_id: 1}, {[metaField]: 1}, {[timeField]: 1}];

function boundsFor(keyPattern, bound) {
    const res = {};
    for (const field of Object.keys(keyPattern)) {
        res[field] = bound;
    }
    return res;
}

// A time-series namespace is rejected at collection acquisition when viewless (our new
// IllegalOperation check). When not running a viewless-only suite, the collection may have been
// created as legacy (viewful) time-series under FCV 8.0, in which case the main namespace is a
// view and the command is rejected earlier as CommandNotSupportedOnView.
// TODO SERVER-131190: Remove the CommandNotSupportedOnView case once 9.0 becomes last LTS.
const kTimeseriesRejectionCodes = isViewlessTimeseriesOnlySuite(db)
    ? [ErrorCodes.IllegalOperation]
    : [ErrorCodes.IllegalOperation, ErrorCodes.CommandNotSupportedOnView];

describe("splitVector/autoSplitVector on time-series collections", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField, metaField}}));
        const docs = [];
        for (let i = 0; i < 100; i++) {
            docs.push({[timeField]: new Date(), [metaField]: i % 5, value: i});
        }
        assert.commandWorked(coll.insert(docs));
    });

    it("splitVector is rejected on a time-series collection", function () {
        // splitVector has no shard-role gate, so it reaches the collection acquisition directly on a
        // mongod, and is routed to the owning shard via mongos. Either way it must be rejected.
        for (const keyPattern of keyPatterns) {
            assert.commandFailedWithCode(
                db.runCommand({splitVector: coll.getFullName(), keyPattern}),
                kTimeseriesRejectionCodes,
                {msg: "splitVector must be rejected on a time-series collection", keyPattern},
            );
        }
    });

    it("autoSplitVector is rejected on a time-series collection", function () {
        // autoSplitVector requires the shard role and cannot be executed against a plain replica
        // set. Skip the case when we are not running against a sharded cluster (e.g. replica set
        // passthrough suites).
        if (!FixtureHelpers.isMongos(db)) {
            jsTest.log.info("Skipping autoSplitVector case: not running against a sharded cluster");
            return;
        }
        for (const keyPattern of keyPatterns) {
            assert.commandFailedWithCode(
                db.runCommand({
                    autoSplitVector: coll.getName(),
                    keyPattern,
                    min: boundsFor(keyPattern, MinKey),
                    max: boundsFor(keyPattern, MaxKey),
                    maxChunkSizeBytes: 1024 * 1024,
                }),
                kTimeseriesRejectionCodes,
                {msg: "autoSplitVector must be rejected on a time-series collection", keyPattern},
            );
        }
    });
});

describe("splitVector/autoSplitVector on a non-existing collection", function () {
    // The time-series rejection check runs before either command's own existence check, so a
    // missing namespace must still surface as NamespaceNotFound rather than being masked or
    // mishandled by the new time-series check.
    const missingColl = db.getCollection(collName + "_missing");

    before(function () {
        missingColl.drop();
    });

    it("autoSplitVector is rejected on a non-existing collection", function () {
        // autoSplitVector requires the shard role and cannot be executed against a plain replica
        // set. Skip the case when we are not running against a sharded cluster (e.g. replica set
        // passthrough suites).
        if (!FixtureHelpers.isMongos(db)) {
            jsTest.log.info("Skipping autoSplitVector case: not running against a sharded cluster");
            return;
        }
        assert.commandFailedWithCode(
            db.runCommand({
                autoSplitVector: missingColl.getName(),
                keyPattern: {_id: 1},
                min: {_id: MinKey},
                max: {_id: MaxKey},
                maxChunkSizeBytes: 1024 * 1024,
            }),
            ErrorCodes.NamespaceNotFound,
            "autoSplitVector must be rejected on a non-existing collection",
        );
    });

    it("splitVector is rejected on a non-existing collection", function () {
        assert.commandFailedWithCode(
            db.runCommand({splitVector: missingColl.getFullName(), keyPattern: {_id: 1}}),
            ErrorCodes.NamespaceNotFound,
            "splitVector must be rejected on a non-existing collection",
        );
    });
});
