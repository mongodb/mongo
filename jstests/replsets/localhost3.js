// Test ReplSet default initiate with localhost-only binding

(function() {
    'use strict';
    const rt =
        new ReplSetTest({name: "foo", nodes: [MongoRunner.mongoOptions({bind_ip: undefined})]});
    const primary = rt.startSet({})[0];
    const db = primary.getDB('admin');
    const resp = assert.commandWorked(db.adminCommand({replSetInitiate: undefined}));
    rt.stopSet();
})();
