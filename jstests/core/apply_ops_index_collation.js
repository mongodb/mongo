// Tests creation of indexes using applyOps for collections with a non-simple default collation.
// Indexes created through applyOps should be built exactly according to their index spec, without
// inheriting the collection default collation, since this is how the oplog entries are replicated.

// @tags: [
//     # Cannot implicitly shard accessed collections because of collection existing when none
//     # expected.
//     assumes_no_implicit_collection_creation_after_drop,
//     requires_non_retryable_commands,
//
//     # applyOps uses the oplog that require replication support
//     requires_replication,
// ]

(function() {
"use strict";

load("jstests/libs/get_index_helpers.js");
load('jstests/libs/uuid_util.js');

const coll = db.apply_ops_index_collation;
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
const uuid = getUUIDFromListCollections(db, coll.getName());

// An index created using a createIndexes-style oplog entry with a non-simple collation does not
// inherit the collection default collation.
let res = db.adminCommand({
    applyOps: [{
        op: "c",
        ns: coll.getFullName(),
        ui: uuid,
        o: {
            createIndexes: coll.getFullName(),
            indexes: [{
                v: 2,
                key: {a: 1},
                name: "a_1_en",
                collation: {
                    locale: "en_US",
                    caseLevel: false,
                    caseFirst: "off",
                    strength: 3,
                    numericOrdering: false,
                    alternate: "non-ignorable",
                    maxVariable: "punct",
                    normalization: false,
                    backwards: false,
                    version: "57.1"
                }
            }],
        }
    }]
});

// It is not possible to test createIndexes in applyOps because that command is not accepted by
// applyOps in that mode.
assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
})();
