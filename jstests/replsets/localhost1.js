// Test ReplSet default initiate with localhost-only binding

(function() {
    'use strict';

    // Select localhost when binding to localhost
    const rt = new ReplSetTest({name: "rsLocal", nodes: 2});
    const primary = rt.startSet({bind_ip: "127.0.0.1"})[0];
    const db = primary.getDB('admin');
    const resp = assert.commandWorked(db.adminCommand({replSetInitiate: undefined}));
    assert(resp.me.startsWith('127.0.0.1'), tojson(resp.me) + " does not start with 127.0.0.1:");
    rt.stopSet();
})();
