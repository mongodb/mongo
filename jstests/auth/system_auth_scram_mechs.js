/**
 * Tests that the __system user can auth using both SCRAM-SHA-1 and SCRAM-SHA-256
 *
 * @tags: [requires_replication]
 */
(function() {
'use strict';

const keyfile = 'jstests/libs/key1';
const keyfileContents = cat(keyfile).replace(/[\011-\015\040]/g, '');
const rs = new ReplSetTest({nodes: 3, keyFile: keyfile});
rs.startSet();
rs.initiate();
const db = rs.getPrimary().getDB("admin");

jsTestLog("Testing scram-sha-256");
assert.eq(db.auth({mechanism: 'SCRAM-SHA-256', user: '__system', pwd: keyfileContents}), 1);
db.logout();

jsTestLog("Testing scram-sha-1");
assert.eq(db.auth({mechanism: 'SCRAM-SHA-1', user: '__system', pwd: keyfileContents}), 1);

rs.stopSet();
})();
