/**
 * Test that mongos creation time is reported once and only once.
 *
 * @tags: [requires_fcv_52]
 */

(function() {
'use strict';

const st = new ShardingTest({shards: 1});

const configDB = st.s.getDB('config');
print("mongos0 connection string: " + st.s0, typeof (st.s0));

let docInitial;
let docNext;
assert.soon(function() {
    const cursor = configDB.mongos.find({_id: st.s0.host});
    if ((cursor.count()) === 0) {
        return false;
    }
    docInitial = cursor.next();
    return docInitial != null;
}, "uptime reporter test timed out on initial", undefined, 1000);
assert.soon(function() {
    const cursor = configDB.mongos.find({_id: st.s0.host});
    if ((cursor.count()) === 0) {
        return false;
    }
    docNext = cursor.next();
    return docNext.created.toString() === docInitial.created.toString() &&
        docNext.ping.toString() != docInitial.ping.toString();
}, "uptime reporter test timed out on update", undefined, 1000);
st.stop();
})();
