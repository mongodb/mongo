/*
 * Tests that when a client sends "helloOk: true" as a part of their isMaster request, mongos
 * will respond with helloOk: true. This ensures that the client knows it can send the hello
 * command.
 *
 * In practice, drivers will send "helloOk: true" in the initial handshake when
 * opening a connection to the database.
 */
(function() {
"use strict";
const st = new ShardingTest({shards: 1, mongos: 1});
const mongos = st.s;

// Simulate an initial handshake request without "helloOk: true". Mongos should not return
// "helloOk: true" in its response.
let res = assert.commandWorked(mongos.adminCommand({isMaster: 1}));
assert.eq(res.helloOk, undefined);

// Simulate an initial handshake request with "helloOk: true". Mongos should now return
// "helloOk: true" in its response.
res = assert.commandWorked(mongos.adminCommand({isMaster: 1, helloOk: true}));
assert.eq("boolean", typeof res.helloOk, "helloOk field is not a boolean" + tojson(res));
assert.eq(res.helloOk, true);

st.stop();
})();
