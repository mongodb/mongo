/* Make sure auth bypass is correctly detected across restarts and user add/delete
 * @tags: [requires_replication, requires_persistence]
 */

(function() {
'use strict';

const keyfile = 'jstests/libs/key1';
const keyfileContents = cat(keyfile).replace(/[\011-\015\040]/g, '');

function createUserCommand(user, roles, wc) {
    return {createUser: user, pwd: 'pwd', roles: roles, writeConcern: wc};
}

function runTest(name, conns, restartCallback) {
    const CREATE_ADMIN = createUserCommand('admin', ['__system'], conns.wc);
    const CREATE_USER1 = createUserCommand('user1', [], conns.wc);
    const CREATE_USER2 = createUserCommand('user2', [], conns.wc);
    const CREATE_USER3 = createUserCommand('user3', [], conns.wc);

    jsTest.log('Starting: ' + name);
    assert(conns.primary);
    let admin = conns.primary.getDB('admin');

    // Initial localhost auth bypass in effect.
    assert.commandWorked(admin.runCommand(CREATE_ADMIN));

    // Localhost auth bypass is now closed.
    assert.commandFailed(admin.runCommand(CREATE_USER1));
    if (conns.replset) {
        assert.commandFailed(conns.replset.getSecondary().getDB('admin').runCommand(CREATE_USER1));
    }

    // But it's okay if we actually auth.
    assert(admin.auth('admin', 'pwd'));
    assert.commandWorked(admin.runCommand(CREATE_USER1));
    admin.logout();

    // Shut down server and restart.
    jsTest.log('Restarting: ' + name);
    conns = restartCallback();
    assert(conns.primary);
    admin = conns.primary.getDB('admin');

    // Localhost auth bypass is still closed.
    assert.commandFailed(admin.runCommand(CREATE_USER2));
    if (conns.replset) {
        assert.commandFailed(conns.replset.getSecondary().getDB('admin').runCommand(CREATE_USER2));
    }

    // We can happily auth and make another user.
    assert(admin.auth('admin', 'pwd'));
    assert.commandWorked(admin.runCommand(CREATE_USER2));

    // We can even drop the collection and our login session will be invalidated.
    const preDrop =
        assert.commandWorked(admin.runCommand({connectionStatus: 1})).authInfo.authenticatedUsers;
    assert.eq(preDrop.length, 1);
    assert.writeOK(admin.system.users.remove({}, {writeConcern: conns.wc}));
    const postDrop =
        assert.commandWorked(admin.runCommand({connectionStatus: 1})).authInfo.authenticatedUsers;
    assert.eq(postDrop.length, 0);

    // Can't recreate ourselves because localhost auth bypass is still disabled.
    assert.commandFailed(admin.runCommand(CREATE_ADMIN));

    jsTest.log('Finished: ' + name);
}

// Node will be bounced. Confirm write goes all the way to disk.
const standaloneWC = {
    w: 1,
    j: true
};
let standalone = MongoRunner.runMongod({auth: '', useHostName: false});
runTest('Standalone', {primary: standalone, wc: standaloneWC}, function() {
    const dbpath = standalone.dbpath;
    MongoRunner.stopMongod(standalone);
    standalone = MongoRunner.runMongod(
        {auth: '', restart: true, cleanData: false, dbpath: dbpath, useHostName: false});
    return {primary: standalone, wc: standaloneWC};
});
MongoRunner.stopMongod(standalone);

const replsetNodes = 2;
// We're going to b bouncing these nodes, make sure writes propagate.
const replsetWC = {
    w: replsetNodes,
    j: true
};
const replset = new ReplSetTest({
    name: 'rs0',
    nodes: replsetNodes,
    nodeOptions: {auth: ''},
    keyFile: keyfile,
    useHostName: false,
});
replset.startSet();
replset.initiate();
replset.awaitSecondaryNodes();
runTest('ReplSet', {primary: replset.getPrimary(), replset: replset, wc: replsetWC}, function() {
    const kAppliedOpTimeTimeoutMS = 10 * 1000;
    // Need to be authed for restart.
    // Only __system is guaranteed to be available, especially during 2nd restart.
    replset.nodes.forEach((node) => assert(node.getDB('admin').auth('__system', keyfileContents)));
    replset.awaitNodesAgreeOnAppliedOpTime(kAppliedOpTimeTimeoutMS, replset.nodes);
    replset.restart(replset.nodes);
    replset.awaitSecondaryNodes();
    return {primary: replset.getPrimary(), replset: replset, wc: replsetWC};
});
replset.stopSet();
})();
