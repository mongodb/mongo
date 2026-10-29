/**
 * Tests that 'recordIdsReplicated' is not exposed in the 'operationDescription' of change stream
 * 'create' events. The field is internal and must not appear in any user-facing interface, and
 * emitting it would make the 'operationDescription' unusable as a 'create' command against a
 * destination that does not recognize the field.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 *   requires_replication,
 *   uses_change_streams,
 * ]
 */

import {hasRecordIdsReplicated} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(jsTestName());
const collName = "coll";

const cursor = testDB.watch([], {showExpandedEvents: true});

assert.commandWorked(testDB.createCollection(collName));

// The collection was indeed created with replicated recordIds, so the 'create' oplog entry carries
// the 'recordIdsReplicated' field that the change stream must scrub.
assert(
    hasRecordIdsReplicated(testDB, collName),
    "expected the collection to be created with replicated recordIds",
);

assert.soon(() => cursor.hasNext(), "expected to observe the 'create' event");
const event = cursor.next();

assert.eq("create", event.operationType, "unexpected event", {event});
assert(
    !event.operationDescription.hasOwnProperty("recordIdsReplicated"),
    "expected 'recordIdsReplicated' to be absent from the create event",
    {event},
);

cursor.close();
rst.stopSet();
