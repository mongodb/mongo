/**
 * Verifies that sharded distinct command parsing and response processing is robust. Designed to
 * reproduce BF-22480.
 *
 * @tags: [requires_sharding]
 */
(function() {
"use strict";
const st = new ShardingTest({shards: 1, mongos: 1});
const dbName = "db";
const db = st.getDB(dbName);
const coll = db[jsTestName()];
const helpFn = function() {
    return "foo";
};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
assert.commandFailed(coll.runCommand("distinct", {help: helpFn, foo: 1}));
assert.commandFailed(coll.runCommand(
    {explain: {distinct: coll.getName(), help: helpFn, foo: 1}, verbosity: 'queryPlanner'}));
st.stop();
})();
