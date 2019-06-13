//
// Basic tests for refineCollectionShardKey.
//

(function() {
    'use strict';

    const st = new ShardingTest({mongos: 1, shards: 2});
    const mongos = st.s;
    const kDbName = 'db';

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: kDbName + '.foo', key: {aKey: 1}}));

    st.stop();
})();
