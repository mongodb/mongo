//
// Basic tests for refineCollectionShardKey.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

const st = new ShardingTest({shards: 1, mongos: 1});
const kDbName = 'db';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const kConfigCollections = 'config.collections';
const kConfigChangelog = 'config.changelog';

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
assert.commandWorked(st.s.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
assert.commandWorked(
    st.s.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));
assert.eq({_id: 1, aKey: 1}, st.s.getCollection(kConfigCollections).findOne({_id: kNsName}).key);
assert.eq(1,
          st.s.getCollection(kConfigChangelog)
              .find({what: 'refineCollectionShardKey.start', ns: kNsName})
              .itcount());
assert.eq(1,
          st.s.getCollection(kConfigChangelog)
              .find({what: 'refineCollectionShardKey.end', ns: kNsName})
              .itcount());
st.stop();
