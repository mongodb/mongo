import "jstests/libs/query/sbe_assert_error_override.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 3}});

var db = st.getDB('test');
db.setSecondaryOk();

assert.commandWorked(db.foo.insert({a: 1}, {writeConcern: {w: 3}}));
assert.commandWorked(db.runCommand(
    {aggregate: 'foo', pipeline: [{$project: {total: {'$add': ['$a', 1]}}}], cursor: {}}));

assert.commandWorked(db.foo.insert({a: [1, 2]}, {writeConcern: {w: 3}}));

var res = db.runCommand(
    {aggregate: 'foo', pipeline: [{$project: {total: {'$add': ['$a', 1]}}}], cursor: {}});
assert.commandFailedWithCode(res, [16554, ErrorCodes.TypeMismatch]);
st.stop();
