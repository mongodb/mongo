/**
 * Tests findAndModify with remove: true and a sort option on a timeseries collection. Sort is
 * supported on unsharded timeseries collections; on sharded timeseries collections the command
 * fails with InvalidOptions because a correct cross-shard sort cannot be guaranteed.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Older binaries reject findAndModify with sort on any timeseries collection.
 *   multiversion_incompatible,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {
    doc1_a_nofields,
    doc4_b_f103,
    doc6_c_f105,
    metaFieldName,
    timeFieldName,
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("findAndModify remove with sort on a timeseries collection", function () {
    it("sorts on an unsharded collection and fails with InvalidOptions on a sharded one", function () {
        const coll = db.getCollection(jsTestName());
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            }),
        );
        assert.commandWorked(coll.insertMany([doc1_a_nofields, doc4_b_f103, doc6_c_f105]));

        // Passthrough suites may implicitly shard accessed collections. Check both the main
        // namespace (viewless timeseries) and the buckets namespace (legacy timeseries).
        const isSharded =
            FixtureHelpers.isSharded(coll) ||
            FixtureHelpers.isSharded(db.getCollection("system.buckets." + coll.getName()));

        const cmd = {
            findAndModify: coll.getName(),
            query: {f: {$gt: 100}},
            remove: true,
            sort: {f: 1},
        };
        if (isSharded) {
            assert.commandFailedWithCode(
                db.runCommand(cmd),
                ErrorCodes.InvalidOptions,
                "expected findAndModify with sort to fail on a sharded timeseries collection",
            );
        } else {
            const res = assert.commandWorked(db.runCommand(cmd));
            assert.eq(1, res.lastErrorObject.n, "expected one document to be removed", {res});
            assert.docEq(
                doc4_b_f103,
                res.value,
                "expected the document with the smallest f to be removed",
            );
            assert.sameMembers(
                [doc1_a_nofields, doc6_c_f105],
                coll.find().toArray(),
                "unexpected remaining documents",
            );
        }
        coll.drop();
    });
});
