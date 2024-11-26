/**
 * When two concurrent identical upsert operations are performed, for which a unique index exists on
 * the query values, it is possible that they will both attempt to perform an insert with one of
 * the two failing on the unique index constraint. This test confirms that we respect collation
 * when deciding to retry the error.
 *
 * @tags: [assumes_unsharded_collection]
 */

const testColl = db.upsert_duplicate_key_retry_collation;

{  // Test index without collation, query with collation
    testColl.drop();
    assert.commandWorked(testColl.createIndex({x: 1}, {unique: true}));
    assert.commandWorked(testColl.insertMany([{_id: 0, x: "UNIQUE"}, {_id: 1, x: "unique"}]));

    assert.throwsWithCode(() => {
        testColl.updateOne({x: "unique"},
                           {$set: {x: "unique"}},
                           {upsert: true, collation: {locale: "en", strength: 1}});
        testColl.updateOne({x: "unique"},
                           {$set: {x: "UNIQUE"}},
                           {upsert: true, collation: {locale: "en", strength: 1}});
    }, ErrorCodes.DuplicateKey);
}

{  // Test index with collation, query without collation
    testColl.drop();
    assert.commandWorked(
        testColl.createIndex({x: 1}, {unique: true, collation: {locale: "en", strength: 2}}));
    assert.commandWorked(testColl.insertMany([{_id: 0, x: "UNIQUE"}]));

    assert.throwsWithCode(
        () => testColl.updateOne({x: "unique"}, {$set: {x: "Unique"}}, {upsert: true}),
        ErrorCodes.DuplicateKey);
}

{  // Test index and query with different collation strength
    testColl.drop();
    assert.commandWorked(
        testColl.createIndex({x: 1}, {unique: true, collation: {locale: "en", strength: 3}}));
    assert.commandWorked(testColl.insertMany([{_id: 0, x: "UNIQUË"}, {_id: 1, x: "UNIQUE"}]));

    assert.throwsWithCode(() => {
        testColl.updateOne({x: "UNIQUE"},
                           {$set: {x: "UNIQUE"}},
                           {upsert: true, collation: {locale: "en", strength: 1}});
        testColl.updateOne({x: "UNIQUE"},
                           {$set: {x: "UNIQUË"}},
                           {upsert: true, collation: {locale: "en", strength: 1}});
    }, ErrorCodes.DuplicateKey);
}
