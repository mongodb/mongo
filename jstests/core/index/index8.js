// Test key uniqueness
//
// @tags: [
//     # The test runs commands that are not allowed with security token: reIndex.
//     not_allowed_with_signed_security_token,
//     # Cannot implicitly shard accessed collections because of not being able to create unique
//     # index using hashed shard key pattern.
//     cannot_create_unique_index_when_using_hashed_shard_key,
//     requires_fastcount,
//     requires_getmore,
// ]

import {IndexUtils} from "jstests/libs/index_utils.js";

let t = db.jstests_index8;
t.drop();

{
    t.createIndex({a: 1});
    t.createIndex({b: 1}, true);
    t.createIndex({c: 1}, [false, "cIndex"]);

    const checkIndexUniqueness = () => {
        IndexUtils.assertIndexes(t, [{_id: 1}, {a: 1}, {b: 1}, {c: 1}]);
        assert(IndexUtils.indexExists(t, {a: 1}, {unique: undefined}), t.getIndexes());
        assert(IndexUtils.indexExists(t, {b: 1}, {unique: true}), t.getIndexes());
        assert(IndexUtils.indexExists(t, {c: 1}, {unique: undefined, name: "cIndex"}), t.getIndexes());
    };

    checkIndexUniqueness();

    // The reIndex command is only supported in standalone mode.
    const hello = db.runCommand({hello: 1});
    const isStandalone = hello.msg !== "isdbgrid" && !hello.hasOwnProperty("setName");
    if (isStandalone) {
        jsTest.log("Re-index and check index key uniqueness");
        assert.commandWorked(t.reIndex());
        checkIndexUniqueness();
    }

    t.save({a: 2, b: 1});
    t.save({a: 2});
    assert.eq(2, t.find().count());

    t.save({b: 4});
    t.save({b: 4});
    assert.eq(3, t.find().count());
    assert.eq(3, t.find().hint({c: 1}).toArray().length);
    assert.eq(3, t.find().hint({b: 1}).toArray().length);
    assert.eq(3, t.find().hint({a: 1}).toArray().length);

    t.drop();
}
{
    t.createIndex({a: 1, b: -1}, true);
    t.save({a: 2, b: 3});
    t.save({a: 2, b: 3});
    t.save({a: 2, b: 4});
    t.save({a: 1, b: 3});
    assert.eq(3, t.find().count());
    t.drop();
}
{
    t.createIndex({a: 1}, true);
    t.save({a: [2, 3]});
    t.save({a: 2});
    assert.eq(1, t.find().count());
    t.drop();
}
{
    t.createIndex({a: 1}, true);
    t.save({a: 2});
    t.save({a: [1, 2, 3]});
    t.save({a: [3, 2, 1]});
    assert.eq(1, t.find().sort({a: 1}).hint({a: 1}).toArray().length);
    assert.eq(1, t.find().sort({a: -1}).hint({a: 1}).toArray().length);

    assert.eq(t._indexSpec({x: 1}, true), t._indexSpec({x: 1}, [true]), "spec 1");
    assert.eq(t._indexSpec({x: 1}, "eliot"), t._indexSpec({x: 1}, ["eliot"]), "spec 2");
}
