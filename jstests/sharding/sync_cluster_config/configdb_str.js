/**
 * Test that mongos with the "wrong" configdb string should not be able to write to config.
 */

(function() {
"use strict";

var st = new ShardingTest({ shards: 1, config: 3, sync: true });
st.stopBalancer();

var badConfStr = st.c1.name + ',' + st.c0.name + ',' + st.c2.name;

var otherMongos = MongoRunner.runMongos({ port: 30998, configdb: badConfStr });
var configDB = otherMongos.getDB('config');

var res = configDB.user.insert({ x: 1 });
assert.writeError(res);
MongoRunner.stopMongos(30998);

st.stop();

})();
