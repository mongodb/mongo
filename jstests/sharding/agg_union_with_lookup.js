/**
 * SERVER-88439 - when run against a sharded collection, this query caused a use after free
 * when running getMore against the returned cursor.
 * @tags: [
 *   # Needs $lookup to support sharded foreign collections.
 *   requires_fcv_51,
 * ]
 */

function insertDocs(coll) {
    assert.commandWorked(coll.insert([
        {v: 1, y: -3},
        {v: 2, y: -2},
        {v: 3, y: -1},
        {v: 4, y: 1},
        {v: 5, y: 2},
        {v: 6, y: 3},
        {v: 7, y: 4}
    ]));
}

const st = new ShardingTest({shards: 2, mongos: 1});

const conn = st.s;

// Setup the collection.
const coll = conn.getCollection("test." + jsTestName() + "_1");
coll.drop();
insertDocs(coll);
assert.commandWorked(coll.createIndex({y: 1}));
st.shardColl(coll,
             /* key */ {y: 1},
             /* split at */ {y: 0},
             /* move chunk containing */ {y: 1},
             /* db */ coll.getDB().getName(),
             /* waitForDelete */ true);

// Construct the command that causes the problem. The problem was observed during the
// subsequent getMore.
const cmd = {
    aggregate: coll.getName(),
    pipeline: [
        {$unionWith: {
            coll: coll.getName(),
            pipeline: [
                {$unionWith: {
                    coll: coll.getName(),
                    pipeline: [
                        {$lookup: {
                            from: coll.getName(),
                            as: "lookedUp",
                            localField: "y",
                            foreignField: "y",
                        }}
                    ],
                }},
            ]
        }},
    ],
    cursor: {batchSize: 1},
};

const initialResult = assert.commandWorked(coll.runCommand(cmd));
const cursor = initialResult.cursor;

const getMore = {
    getMore: cursor.id,
    collection: coll.getName(),
    batchSize: 1000
};
const getMoreResult = assert.commandWorked(coll.runCommand(getMore));
const allResults = cursor.firstBatch.concat(getMoreResult.cursor.nextBatch);

assert.eq(allResults.length, 21);

st.stop();
