/**
 * This test checks the upgrade path from noauth/allowTLS to x509/requireTLS
 *
 * NOTE: This test is similar to upgrade_noauth_to_x509_ssl.js in the ssl test
 * suite. This test cannot use ssl communication and therefore cannot test
 * modes that only allow ssl.
 *
 * This test requires data to persist across a restart.
 * @tags: [requires_persistence]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {allowTLS} from "jstests/ssl/libs/ssl_helpers.js";

var dbName = 'upgradeToX509';

// Disable auth explicitly
var noAuth = {noauth: ''};

// Undefine the flags we're replacing, otherwise upgradeSet will keep old values.
var transitionToX509allowTLS =
    Object.merge(allowTLS, {noauth: undefined, transitionToAuth: '', clusterAuthMode: 'x509'});

var rst = new ReplSetTest({name: 'noauthSet', nodes: 3, nodeOptions: noAuth});
rst.startSet();
rst.initiate();

var testDB = rst.getPrimary().getDB(dbName);
assert.commandWorked(testDB.a.insert({a: 1, str: 'TESTTESTTEST'}));
assert.eq(1, testDB.a.find().itcount(), 'Error interacting with replSet');

print('=== UPGRADE no-auth/no-ssl -> transition to X509/allowTLS ===');
rst.upgradeSet(transitionToX509allowTLS);

// Connect to the new primary
testDB = rst.getPrimary().getDB(dbName);
assert.commandWorked(testDB.a.insert({a: 1, str: 'TESTTESTTEST'}));
assert.eq(2, testDB.a.find().itcount(), 'Error interacting with replSet');

rst.stopSet();
