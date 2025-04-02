/**
 * Tests that when initial syncing a collection with recordIdsReplicated:true, the recordIds
 * are preserved across the logical initial sync.
 *
 * @tags: [
 *     featureFlagRecordIdsReplicated
 * ]
 */

import {
    testPreservingRecordIdsDuringInitialSync,
} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";

testPreservingRecordIdsDuringInitialSync(
    "logical", "initialSyncHangBeforeCopyingDatabases", "initialSyncHangAfterDataCloning");
