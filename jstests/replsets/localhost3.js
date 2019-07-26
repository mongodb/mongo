// Test ReplSet default initiate with localhost-only binding

(function() {
'use strict';

// Select localhost when binding to localhost
const rt = new ReplSetTest({name: "rsLocal", nodes: 1});
const primary = rt.startSet({bind_ip: undefined})[0];
const db = primary.getDB('admin');
const resp = assert.commandWorked(db.adminCommand({replSetInitiate: undefined}));
assert(resp.me.startsWith('localhost:'), tojson(resp.me) + " should start with localhost:");

// Wait for the primary to complete its election before shutting down the set.
assert.soon(() => db.runCommand({ismaster: 1}).ismaster);
rt.stopSet();
})();
