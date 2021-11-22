/*
 * Test to reproduce bug described by SERVER-60533.
 *
 * @tags: [requires_fcv_52]
 */
(function() {
"use strict";
var st = new ShardingTest({shards: 2});
const coll = st.s.getDB("test").getCollection("distinct");
coll.drop();
assert.eq(0, coll.distinct("a").length, "Ensure distinct works on an empty collection.");
st.stop();
})();
