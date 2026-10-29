/**
 * Tests that the '$_remainingQuota' aggregate argument limits the number of bytes a $out stage may
 * write.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_stepdowns,
 *   requires_fcv_91, # $_remainingQuota added in 9.1
 * ]
 */
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("$out with $_remainingQuota", function () {
    const kNumDocs = 10;
    const kQuotaExceededCode = 13624400;

    let testDb;
    let source;
    let target;
    let totalSize;

    function getOutMetrics() {
        const out = assert.commandWorked(testDb.adminCommand({serverStatus: 1})).metrics.query.out;
        return {
            invokedWithQuota: Number(out.countInvokedWithQuota),
            failedOverQuota: Number(out.countFailedOverQuota),
        };
    }

    /**
     * Runs $out and asserts that the serverStatus counters changed by exactly the expected amounts.
     * Returns the command response so the caller can check its outcome.
     */
    function runOut(extraArgs, expectedDeltas) {
        const before = getOutMetrics();
        const res = testDb.runCommand({
            aggregate: source.getName(),
            pipeline: [{$match: {}}, {$out: target.getName()}],
            cursor: {},
            ...extraArgs,
        });
        const after = getOutMetrics();
        assert.eq(
            expectedDeltas.invokedWithQuota,
            after.invokedWithQuota - before.invokedWithQuota,
            "unexpected change in countInvokedWithQuota",
            {before, after},
        );
        assert.eq(
            expectedDeltas.failedOverQuota,
            after.failedOverQuota - before.failedOverQuota,
            "unexpected change in countFailedOverQuota",
            {before, after},
        );
        return res;
    }

    before(function () {
        testDb = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDb.dropDatabase());
        source = testDb.source;
        target = testDb.target;

        // Seed the source collection with documents of a known, identical size.
        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({_id: i, payload: "x".repeat(1000)});
        }
        assert.commandWorked(source.insertMany(docs));
        totalSize = docs.reduce((sum, doc) => sum + Object.bsonsize(doc), 0);
    });

    afterEach(function () {
        target.drop();
    });

    after(function () {
        assert.commandWorked(testDb.dropDatabase());
    });

    it("succeeds when $_remainingQuota is unset", function () {
        assert.commandWorked(runOut({}, {invokedWithQuota: 0, failedOverQuota: 0}));
        assert.eq(kNumDocs, target.countDocuments({}));
    });

    it("succeeds when $_remainingQuota is larger than the collection size", function () {
        assert.commandWorked(
            runOut(
                {$_remainingQuota: NumberLong(totalSize + 1000)},
                {invokedWithQuota: 1, failedOverQuota: 0},
            ),
        );
        assert.eq(kNumDocs, target.countDocuments({}));
    });

    it("fails when $_remainingQuota is smaller than the collection size", function () {
        assert.commandFailedWithCode(
            runOut(
                {$_remainingQuota: NumberLong(totalSize - 1)},
                {invokedWithQuota: 1, failedOverQuota: 1},
            ),
            kQuotaExceededCode,
        );
        assert.eq(0, target.countDocuments({}));
    });
});
