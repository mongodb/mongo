/**
 * This test is intended to exercise shard filtering logic. This test works by sharding a
 * collection, and then inserting orphaned documents directly into one of the shards. It then runs a
 * find() and makes sure that orphaned documents are filtered out.
 * @tags: [requires_sharding]
 */
(function() {
"use strict";

[false, true].forEach((isSbe) => {
    // Deliberately inserts orphans outside of migration.
    TestData.skipCheckOrphans = true;
    const st = new ShardingTest({
        shards: 2,
        other:
            {shardOptions: {setParameter: `internalQueryEnableSlotBasedExecutionEngine=${isSbe}`}}
    });
    const collName = "test.shardfilter";
    const shard0Coll = st.s.getCollection(collName);

    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    st.ensurePrimaryShard("test", st.shard1.name);
    assert.commandWorked(
        st.s.adminCommand({shardcollection: collName, key: {'a': 1, 'b.c': 1, 'd.e.f': 1}}));

    // Shard the collection and insert some docs.
    const docs = [
        {a: 1, b: {c: 1}, d: {e: {f: 1}}, g: 100},
        {a: 1, b: {c: 2}, d: {e: {f: 2}}, g: 100.9},
        {a: 1, b: {c: 3}, d: {e: {f: 3}}, g: "a"},
        {a: 1, b: {c: 3}, d: {e: {f: 3}}, g: [1, 2, 3]},
        {a: "a", b: {c: "b"}, d: {e: {f: "c"}}, g: null},
        {a: 1.0, b: {c: "b"}, d: {e: {f: Infinity}}, g: NaN}
    ];
    assert.commandWorked(st.getDB("test").shardfilter.insert(docs));
    assert.eq(st.getDB("test").shardfilter.find().itcount(), 6);

    // Insert some documents with valid partial shard keys to both shards.
    const docsWithMissingAndNullKeys = [
        {a: "missingParts"},
        {a: null, b: {c: 1}, d: {e: {f: 1}}},
        {a: "null", b: {c: null}, d: {e: {f: 1}}},
        {a: "deepNull", b: {c: 1}, d: {e: {f: null}}},
    ];
    assert.commandWorked(st.shard0.getCollection(collName).insert(docsWithMissingAndNullKeys));
    assert.commandWorked(st.shard1.getCollection(collName).insert(docsWithMissingAndNullKeys));

    // Insert docs without missing or null shard keys onto shard0 and test that they get filtered
    // out.
    const orphanDocs = [
        {a: 100, b: {c: 10}, d: {e: {f: 999}}, g: "a"},
        {a: 101, b: {c: 11}, d: {e: {f: 1000}}, g: "b"}
    ];
    assert.commandWorked(st.shard0.getCollection(collName).insert(orphanDocs));
    assert.eq(st.getDB("test").shardfilter.find().itcount(), 10);

    // Insert docs directly into shard0 to test that regular (non-null, non-missing) shard keys get
    // filtered out.
    assert.commandWorked(st.shard0.getCollection(collName).insert(docs));
    assert.eq(st.getDB("test").shardfilter.find().itcount(), 10);

    st.stop();
});
})();
