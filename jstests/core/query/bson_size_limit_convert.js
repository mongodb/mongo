/**
 * Tests that $convert to BinData accepts a document of exactly the max user size as an intermediary.
 *
 * @tags: [
 *     # Overflows WT cache on in-memory variants.
 *     requires_persistence,
 *     requires_getmore,
 *     # Inserts large documents into a single transaction, causing
 *     # TransactionTooLargeForCache when run under the multi-statement txn passthrough.
 *     does_not_support_transactions,
 *     # Inserts documents that are too large for a timeseries collection.
 *     exclude_from_timeseries_crud_passthrough,
 *     # Inserts large documents, causing severe WiredTiger eviction pressure on the
 *     # initial sync node and making the primary unreachable.
 *     incompatible_with_initial_sync,
 *     # Creates memory pressure on secondaries in some passthroughs (esp. secondary_reads_passthrough)
 *     # under burn-in configuration.
 *     assumes_read_preference_unchanged,
 *     # Inserts large documents; must run in the resource_intensive bucket so it is
 *     # not run in parallel with other tests on memory-constrained machines.
 *     resource_intensive,
 *     # Uses the new $convert on a document feature.
 *     requires_fcv_90,
 * ]
 */

import {it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];
coll.drop();

const bsonMaxSizeLimit = 16 * 1024 * 1024;
// Insert a document that is exactly 16 MB.
const maxSizeDoc = {_id: 1, x: "a".repeat(bsonMaxSizeLimit - 26)};
assert.eq(Object.bsonsize(maxSizeDoc), bsonMaxSizeLimit);
assert.commandWorked(coll.insert(maxSizeDoc));

// Size limit: does not trigger. A $convert should be able to turn a document of
// exactly max user size into BinData, which is then passed over to a hash function.
// Before SERVER-135206, BinData would throw without allowing this valid intermediary
// document.
it("$convert to BinData of a max-size document", () => {
    const res = coll
        .aggregate([
            {
                $project: {
                    _id: 0,
                    hash: {
                        $hash: {
                            algorithm: "xxh64",
                            input: {$convert: {input: "$$ROOT", to: "binData"}},
                        },
                    },
                },
            },
        ])
        .toArray();
    assert.eq(res.length, 1, res);
});
