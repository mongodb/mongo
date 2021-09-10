/*
 * Tests for an empty readConcern object, passed as not the first
 * statement in a multi-statement transaction.
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
"use strict";

const lsid = {
    id: UUID()
};

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const collection = primary.getCollection("test.mycoll");
    assert.commandWorked(collection.insert({}));

    const commandObj = {
        find: collection.getName(),
        lsid,
        txnNumber: NumberLong(1),
        autocommit: false,
    };

    assert.commandWorked(
        collection.runCommand(Object.assign({}, commandObj, {startTransaction: true})));

    assert.commandWorked(collection.runCommand(Object.assign({}, commandObj, {readConcern: {}})));

    rst.stopSet();
}

{
    const st = new ShardingTest({mongos: 1, config: 1, shards: 1, rs: {nodes: 1}});
    const collection = st.s.getCollection("test.mycoll");
    assert.commandWorked(collection.insert({}));

    const commandObj = {
        find: collection.getName(),
        lsid,
        txnNumber: NumberLong(1),
        autocommit: false,
    };

    assert.commandWorked(
        collection.runCommand(Object.assign({}, commandObj, {startTransaction: true})));

    assert.commandWorked(collection.runCommand(Object.assign({}, commandObj, {readConcern: {}})));

    st.stop();
}
})();
