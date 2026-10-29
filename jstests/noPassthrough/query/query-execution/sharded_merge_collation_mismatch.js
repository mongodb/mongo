/**
 * Tests that $merge into a sharded collection fails when source and destination collection collations differ.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, config: 1});
try {
    const dbName = jsTestName();
    const db = st.s.getDB(dbName);
    const src = db.src;
    const dst = db.dst;
    const dstNs = dbName + ".dst";
    const caseInsensitive = {locale: "en_US", strength: 2};

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );

    // Both directions of collation mismatch should fail.
    for (const [srcOpts, dstOpts] of [
        [{collation: caseInsensitive}, null],
        [null, {collation: caseInsensitive}],
    ]) {
        src.drop();
        dst.drop();
        assert.commandWorked(db.createCollection(src.getName(), srcOpts || {}));
        assert.commandWorked(db.createCollection(dst.getName(), dstOpts || {}));
        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: dstNs,
                key: {sk: 1},
                collation: {locale: "simple"},
            }),
        );
        const err = assert.throws(() => src.aggregate({$merge: {into: dst.getName()}}));
        assert.commandFailedWithCode(err, [51183, 11749300]);
    }

    {
        src.drop();
        dst.drop();
        assert.commandWorked(db.createCollection(src.getName(), {collation: caseInsensitive}));
        assert.commandWorked(db.createCollection(dst.getName(), {collation: caseInsensitive}));
        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: dstNs,
                key: {sk: 1},
                collation: {locale: "simple"},
            }),
        );
        // TODO this should fail but it succeeds
        assert.doesNotThrow(() => src.aggregate({$merge: {into: dst.getName()}}));
    }

    // Explicit 'on' fields with a matching unique index should succeed.
    {
        src.drop();
        dst.drop();
        assert.commandWorked(db.createCollection(dst.getName(), {collation: caseInsensitive}));
        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: dstNs,
                key: {sk: 1},
                collation: {locale: "simple"},
            }),
        );
        assert.commandWorked(
            dst.createIndex({sk: 1, k: 1}, {unique: true, collation: {locale: "simple"}}),
        );
        assert.commandWorked(db.createCollection(src.getName()));
        assert.commandWorked(src.insert({_id: "s1", sk: "a", k: "x"}));
        assert.commandWorked(dst.insert({_id: "d1", sk: "a", k: "y"}));
        assert.doesNotThrow(() => src.aggregate({$merge: {into: dst.getName(), on: ["sk", "k"]}}));
    }

    {
        src.drop();
        dst.drop();
        assert.commandWorked(db.createCollection(src.getName()));
        assert.commandWorked(db.createCollection(dst.getName(), {collation: caseInsensitive}));
        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: dstNs,
                key: {sk: 1, _id: 1},
                collation: {locale: "simple"},
            }),
        );
        const err = assert.throws(() => src.aggregate({$merge: {into: dst.getName()}}));
        assert.commandFailedWithCode(err, [51183, 11749300]);
    }

    {
        src.drop();
        dst.drop();
        assert.commandWorked(db.createCollection(src.getName()));
        assert.commandWorked(src.insert({_id: "a"}));
        assert.commandWorked(src.insert({_id: "A"}));
        assert.commandWorked(db.createCollection(dst.getName(), {collation: caseInsensitive}));
        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: dstNs,
                key: {sk: 1},
                collation: {locale: "simple"},
            }),
        );
        assert.commandWorked(dst.insert({_id: "A"}));
        const err = assert.throws(() =>
            src.aggregate([{$merge: {into: dst.getName()}}], {collation: caseInsensitive}),
        );
        assert.commandFailedWithCode(err, ErrorCodes.ImmutableField);
    }

    // Unsharded destination: the mongos collation check only covers tracked
    // collections, so this mismatch is not caught here.
    {
        src.drop();
        dst.drop();
        assert.commandWorked(db.createCollection(src.getName()));
        assert.commandWorked(db.createCollection(dst.getName(), {collation: caseInsensitive}));
        assert.doesNotThrow(() => src.aggregate({$merge: {into: dst.getName()}}));
    }
} finally {
    st.stop();
}
