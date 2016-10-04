/**
 * This test checks the upgrade path from noauth/allowSSL to x509/requireSSL
 *
 * NOTE: This test is similar to upgrade_noauth_to_x509_ssl.js in the ssl test
 * suite. This test cannot use ssl communication and therefore cannot test
 * modes that only allow ssl.
 *
 * This test requires data to persist across a restart.
 * @tags: [requires_persistence]
 */

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';
    var dbName = 'upgradeToX509';

    // Disable auth explicitly
    var noAuth = {noauth: ''};

    // Undefine the flags we're replacing, otherwise upgradeSet will keep old values.
    var transitionToX509AllowSSL =
        Object.merge(allowSSL, {noauth: undefined, transitionToAuth: '', clusterAuthMode: 'x509'});

    var rst = new ReplSetTest({name: 'noauthSet', nodes: 3, nodeOptions: noAuth});
    rst.startSet();
    rst.initiate();

    var testDB = rst.getPrimary().getDB(dbName);
    assert.writeOK(testDB.a.insert({a: 1, str: 'TESTTESTTEST'}));
    assert.eq(1, testDB.a.count(), 'Error interacting with replSet');

    print('=== UPGRADE no-auth/no-ssl -> transition to X509/allowSSL ===');
    rst.upgradeSet(transitionToX509AllowSSL);

    // Connect to the new primary
    testDB = rst.getPrimary().getDB(dbName);
    assert.writeOK(testDB.a.insert({a: 1, str: 'TESTTESTTEST'}));
    assert.eq(2, testDB.a.count(), 'Error interacting with replSet');

    rst.stopSet();
}());
