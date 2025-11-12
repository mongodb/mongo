// Testing indexes in nested fields.

import {IndexUtils} from "jstests/libs/index_utils.js";

let t = db.index4;
t.drop();

t.save({name: "alleyinsider", instances: [{pool: "prod1"}, {pool: "dev1"}]});

t.save({name: "clusterstock", instances: [{pool: "dev1"}]});

// this should fail, not allowed -- we confirm that.
assert.commandFailed(t.createIndex({instances: {pool: 1}}));
IndexUtils.assertIndexes(t, [{_id: 1}], "no indexes other than _id should be here yet");

assert.commandWorked(t.createIndex({"instances.pool": 1}));
IndexUtils.assertIndexes(t, [{_id: 1}, {"instances.pool": 1}]);

sleep(10);

let a = t.find({instances: {pool: "prod1"}});
assert(a.length() == 1, "len1");
assert(a[0].name == "alleyinsider", "alley");

assert(t.validate().valid, "valid");
