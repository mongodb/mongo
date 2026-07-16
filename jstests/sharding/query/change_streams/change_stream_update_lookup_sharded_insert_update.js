// Tests that a change stream on a sharded collection, started from a cluster time before an
// insert and a subsequent update, returns the expected 'insert' and 'update' events, and that
// 'fullDocument: "updateLookup"' returns the fully updated document for the 'update' event.
//
// @tags: [
//   assumes_balancer_off,
//   # If a rollback is triggered during a stepdown, the change stream cursor can become invalid.
//   does_not_support_stepdowns,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

describe("change stream update lookup on sharded collection with agg pipeline", () => {
    let st;
    let db;
    let coll;

    before(() => {
        st = new ShardingTest({
            shards: 2,
            rs: {
                nodes: 1,
                // Use a higher frequency for periodic noops to speed up the test.
                setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
            },
        });

        db = st.s0.getDB(jsTestName());

        // Ensure the test db primary is st.shard0.shardName.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.rs0.getURL()}));

        coll = db[jsTestName()];
    });

    after(() => {
        st.stop();
    });

    beforeEach(() => {
        // Shard the test collection on 'sk'.
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}));
    });

    afterEach(() => {
        assertDropCollection(db, coll.getName());
    });

    it("prohibits storing an _id field with dollar-prefixed sub-fields", () => {
        // Use an _id value that contains a dollar-prefixed field name. This should fail.
        assert.commandFailedWithCode(
            coll.insert({_id: {$v: 2}, sk: 1, val: "original"}),
            ErrorCodes.DollarPrefixedFieldName,
        );
    });

    const testCases = [
        // Case: document is placed on same shard as the "update" oplog entry.
        {
            name: "local lookup",
            doc: {_id: 1, sk: {$type: 2}, val: "original"},
            prepare: () => {},
        },
        // Case: document is placed on different shard than the "update" oplog entry.
        {
            name: "remote lookup",
            doc: {_id: 1, sk: {$type: 2}, val: "original"},
            prepare: () => {
                // Split into 2 chunks and move them both to shard1. The document is then present on shard1,
                // but the oplog entries are on the primary shard (shard0).
                assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {sk: {test: 0}}}));
                [-1, 1].forEach((splitPoint) => {
                    assert.commandWorked(
                        db.adminCommand({
                            moveChunk: coll.getFullName(),
                            find: {sk: {test: splitPoint}},
                            to: st.rs1.getURL(),
                            _waitForDelete: true,
                        }),
                    );
                });
            },
        },
        // Case: using a dollar-prefixed field name that does not exist as an MQL operator.
        {
            name: "unknown operator",
            doc: {_id: 1, sk: {$operatorDoesNotExist: 2}, val: "original"},
            prepare: () => {},
        },
    ];

    for (const {name, doc, prepare} of testCases) {
        it(`returns expected full document, using dollar-prefixed shard key field, ${name}`, () => {
            const startTime = getClusterTime(db);

            assert.commandWorked(coll.insert(doc));

            // Make sure document can be retrieved back.
            const lookup = coll.find({_id: doc._id}).toArray();
            assert.eq(1, lookup.length);
            assert.eq(lookup[0], doc);

            // Run an update operation. This will make the updateLookup use the _id and shard key values for
            // looking up the document.
            assert.commandWorked(coll.update({_id: doc._id}, {$set: {val: "changed"}}));

            // Force usage of the aggregation pipeline executor for updateLookup.
            prepare();

            const changeStream = coll.watch([], {
                startAtOperationTime: startTime,
                fullDocument: "updateLookup",
            });

            assert.soon(() => changeStream.hasNext());
            let next = changeStream.next();
            assert.eq(next.operationType, "insert", {next});
            assert.docEq(doc, next.fullDocument);

            assert.soon(() => changeStream.hasNext());
            next = changeStream.next();
            assert.eq(next.operationType, "update", {next});
            assert.docEq(Object.assign({}, doc, {val: "changed"}), next.fullDocument);

            changeStream.close();
        });
    }
});
