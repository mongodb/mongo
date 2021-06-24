/**
 * Verifies mongos and mongod's behavior for exhaust queries.
 *
 * 'ShardingTest' requires replication.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const docs = [{a: 1}, {a: 2}, {a: 3}, {a: 4}, {a: 5}, {a: 6}, {a: 7}, {a: 8}];
const kBatchSize = 3;
const kNoOfDocs = docs.length;

[{
    "setUp": () => {
        const conn = MongoRunner.runMongod();
        const db = conn.getDB(jsTestName());
        return {"env": conn, "db": db};
    },
    "tearDown": (env) => MongoRunner.stopMongod(env),
    "verifyThis": (cursor, docIdx, doc) => {
        // Because the first batch is returned from a find command without exhaustAllowed bit,
        // moreToCome bit is not set in reply message.
        const isFirstBatch = docIdx < kBatchSize;

        // The last batch which does not contain the full batch size is returned without moreToCome
        // bit set.
        const isLastBatch = docIdx >= kNoOfDocs - (kNoOfDocs % kBatchSize);

        if (isFirstBatch || isLastBatch) {
            assert(!cursor._hasMoreToCome(), `${docIdx} doc: ${doc}`);
        } else {
            assert(cursor._hasMoreToCome(), `${docIdx} doc: ${doc}`);
        }
    }
},
 {
     "setUp": () => {
         const st = new ShardingTest({shards: 1, config: 1});
         const db = st.s0.getDB(jsTestName());
         return {"env": st, "db": db};
     },
     "tearDown": (env) => env.stop(),
     "verifyThis": (cursor, docIdx, doc) => {
         // Mongos does not support exhaust queries, not by returning an error but by sending reply
         // without moreToCome bit set. So, _hasMoreToCome() is always false.
         assert(!cursor._hasMoreToCome(), `${docIdx} doc: ${doc}`);
     }
 }].forEach(({setUp, tearDown, verifyThis}) => {
    const {env, db} = setUp();

    db.coll.drop();

    assert.commandWorked(db.coll.insert(docs));

    let cursor = db.coll.find().batchSize(kBatchSize).addOption(DBQuery.Option.exhaust);
    let docIdx = 0;
    cursor.forEach(doc => {
        verifyThis(cursor, docIdx, doc);
        ++docIdx;
    });

    tearDown(env);
});
}());
