/**
 * Test that verifies the behavior of merge sort when memory limit is set.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_83,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   # Ignore because the find command is rewritten for TS collections before reaching the failpoint.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const kDocCount = 100000;

function insert_docs_and_create_indexes(coll) {
    const kBulkSize = 10000;

    coll.drop();
    for (let i = 0; i < kDocCount; i += kBulkSize) {
        const bulk = coll.initializeUnorderedBulkOp();
        for (let j = 0; j < kBulkSize && i + j < kDocCount; j++) {
            const docId = i + j;
            bulk.insert({
                _id: docId,
                a: docId,
                b: docId,
            });
        }
        assert.commandWorked(bulk.execute());
    }

    assert.commandWorked(coll.createIndex({a: 1}));
}

function assert_cursor(cursor, docCount, expectFailure) {
    if (expectFailure) {
        const kErrorCodes = [11130300, 11130301, 11130303];
        assert.throwsWithCode(
            () => {
                cursor.itcount();
            },
            kErrorCodes,
            [] /*params*/,
            () => cursor.explain("executionStats"),
        );
    } else {
        assert.eq(cursor.itcount(), docCount, () => cursor.explain("executionStats"));
    }
}

function run_tests(coll, expectFailure) {
    assert_cursor(
        coll
            .find({
                $or: [
                    {a: {$gte: 0}, b: {$mod: [2, 0]}},
                    {a: {$lte: kDocCount}, b: {$mod: [2, 1]}},
                ],
            })
            .sort({a: 1}),
        kDocCount,
        expectFailure,
    );
}

const coll = db.merge_sort_memory_limit;
insert_docs_and_create_indexes(coll);

run_tests(coll, false /*expectFailure*/);

const originalMergeSortStageMemory = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalMergeSortStageMaxMemoryBytes: 1}),
);
const originalUniqueStageMemory = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalSlotBasedExecutionUniqueStageMaxMemoryBytes: 1}),
);

const kMemoryLimit = 256 * 1024;
setParameterOnAllNonConfigNodes(db.getMongo(), "internalMergeSortStageMaxMemoryBytes", kMemoryLimit);
setParameterOnAllNonConfigNodes(db.getMongo(), "internalSlotBasedExecutionUniqueStageMaxMemoryBytes", kMemoryLimit);

run_tests(coll, true /*expectFailure*/);

setParameterOnAllNonConfigNodes(
    db.getMongo(),
    "internalMergeSortStageMaxMemoryBytes",
    originalMergeSortStageMemory.internalMergeSortStageMaxMemoryBytes,
);
setParameterOnAllNonConfigNodes(
    db.getMongo(),
    "internalSlotBasedExecutionUniqueStageMaxMemoryBytes",
    originalUniqueStageMemory.internalSlotBasedExecutionUniqueStageMaxMemoryBytes,
);
