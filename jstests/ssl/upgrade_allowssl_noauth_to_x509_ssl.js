/**
 * This test checks the upgrade path from noauth/allowSSL to x509/requireSSL
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';

    // Disable auth explicitly
    var noAuthAllowSSL = Object.merge(allowSSL, {noauth: ''});

    // Undefine the flags we're replacing, otherwise upgradeSet will keep old values.
    var tryX509preferSSL =
        Object.merge(preferSSL, {noauth: undefined, tryClusterAuth: '', clusterAuthMode: 'x509'});
    var x509RequireSSL =
        Object.merge(requireSSL, {tryClusterAuth: undefined, clusterAuthMode: 'x509'});

    var rst = new ReplSetTest({name: 'noauthSet', nodes: 3, nodeOptions: noAuthAllowSSL});
    rst.startSet();
    rst.initiate();

    var rstConn1 = rst.getPrimary();
    // Create a user to login when auth is enabled later
    rstConn1.getDB('admin').createUser({user: 'root', pwd: 'root', roles: ['root']});

    rstConn1.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'});
    assert.eq(1, rstConn1.getDB('test').a.count(), 'Error interacting with replSet');

    print('=== UPGRADE no-auth/allowSSL -> try X509/preferSSL ===');
    rst.upgradeSet(tryX509preferSSL);
    var rstConn2 = rst.getPrimary();
    rstConn2.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'});
    assert.eq(2, rstConn2.getDB('test').a.count(), 'Error interacting with replSet');

    print('=== UPGRADE try X509/preferSSL -> X509/requireSSL ===');
    rst.upgradeSet(x509RequireSSL, 'root', 'root');

    // upgradeSet leaves its connections logged in as root
    var rstConn3 = rst.getPrimary();
    rstConn3.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'});
    assert.eq(3, rstConn3.getDB('test').a.count(), 'Error interacting with replSet');

    rst.stopSet();
}());
