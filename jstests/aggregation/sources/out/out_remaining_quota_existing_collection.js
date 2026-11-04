/**
 * Tests that when '$_remainingQuota' is set and $out writes to an existing collection, the size of
 * the collection being overwritten is credited back against the quota.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_stepdowns,
 *   requires_fcv_91, # $_remainingQuota added in 9.1
 * ]
 */
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("$out to an existing collection with $_remainingQuota", function () {
    const kNumSourceDocs = 10;
    const kQuotaExceededCode = 13624400;

    let testDb;
    let source;
    let target;

    function makeDocs(numDocs) {
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, payload: "x".repeat(1000)});
        }
        return docs;
    }

    function getOutMetrics() {
        const out = assert.commandWorked(testDb.adminCommand({serverStatus: 1})).metrics.query.out;
        return {
            invokedWithQuota: Number(out.countInvokedWithQuota),
            failedOverQuota: Number(out.countFailedOverQuota),
        };
    }

    /**
     * Runs $out with a remaining quota of 0 and asserts that the serverStatus counters changed by
     * exactly the expected amounts. Returns the command response.
     */
    function runOutWithZeroQuota(expectedFailedOverQuota) {
        const before = getOutMetrics();
        const res = testDb.runCommand({
            aggregate: source.getName(),
            pipeline: [{$match: {}}, {$out: target.getName()}],
            cursor: {},
            $_remainingQuota: NumberLong(0),
        });
        const after = getOutMetrics();
        assert.eq(
            1,
            after.invokedWithQuota - before.invokedWithQuota,
            "unexpected countInvokedWithQuota delta",
            {
                before,
                after,
            },
        );
        assert.eq(
            expectedFailedOverQuota,
            after.failedOverQuota - before.failedOverQuota,
            "unexpected countFailedOverQuota delta",
            {before, after},
        );
        return res;
    }

    before(function () {
        testDb = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDb.dropDatabase());
        source = testDb.source;
        target = testDb.target;
        assert.commandWorked(source.insertMany(makeDocs(kNumSourceDocs)));
    });

    afterEach(function () {
        target.drop();
    });

    after(function () {
        assert.commandWorked(testDb.dropDatabase());
    });

    it("succeeds with zero quota when the existing collection is larger than the output", function () {
        assert.commandWorked(target.insertMany(makeDocs(kNumSourceDocs * 2)));

        assert.commandWorked(runOutWithZeroQuota(0));
        assert.eq(kNumSourceDocs, target.countDocuments({}));
    });

    it("fails with zero quota when the existing collection is smaller than the output", function () {
        const kNumExistingDocs = kNumSourceDocs / 2;
        assert.commandWorked(target.insertMany(makeDocs(kNumExistingDocs)));

        assert.commandFailedWithCode(runOutWithZeroQuota(1), kQuotaExceededCode);
        // The existing collection must be left untouched.
        assert.eq(kNumExistingDocs, target.countDocuments({}));
    });
});
