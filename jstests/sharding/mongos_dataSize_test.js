// This tests the command dataSize on sharded clusters to ensure that they can use the command.

(function() {
'use strict';

let s = new ShardingTest({shards: 2, mongos: 1});
let db = s.getDB("test");
assert.commandWorked(s.s0.adminCommand({enableSharding: "test"}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));
assert.commandWorked(s.getPrimaryShard("test").getDB("admin").runCommand({datasize: "test.foo"}));
assert.commandFailedWithCode(s.getPrimaryShard("test").getDB("admin").runCommand({datasize: "foo"}),
                             ErrorCodes.InvalidNamespace);
s.stop();
})();
