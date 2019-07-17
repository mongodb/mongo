// Test ReplSet default initiate with 0.0.0.0 binding

(function() {
    'use strict';

    // Select localhost when binding to localhost
    const rt = new ReplSetTest({name: "rsLocal", nodes: 1});
    const primary = rt.startSet({bind_ip: "0.0.0.0"})[0];
    const db = primary.getDB('admin');
    const resp = assert.commandWorked(db.adminCommand({replSetInitiate: undefined}));
    assert(!resp.me.startsWith('127.0.0.1:'),
           tojson(resp.me) + " should not start with 127.0.0.1:");
    assert(!resp.me.startsWith('0.0.0.0:'), tojson(resp.me) + " should not start with 0.0.0.0:");
    assert(!resp.me.startsWith('localhost:'),
           tojson(resp.me) + " should not start with localhost:");

    // Wait for the primary to complete its election before shutting down the set.
    assert.soon(() => db.runCommand({ismaster: 1}).ismaster);
    rt.stopSet();
})();
