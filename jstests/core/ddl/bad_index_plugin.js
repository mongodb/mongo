// SERVER-5826 ensure you can't build an index with a non-existent plugin
import {IndexUtils} from "jstests/libs/index_utils.js";

let t = db.bad_index_plugin;

assert.commandWorked(t.createIndex({good: 1}));
IndexUtils.assertIndexes(t, [{_id: 1}, {good: 1}]);

let err = t.createIndex({bad: "bad"});
assert.commandFailed(err);
assert(err.code >= 0);

IndexUtils.assertIndexes(t, [{_id: 1}, {good: 1}]); // (no bad)
