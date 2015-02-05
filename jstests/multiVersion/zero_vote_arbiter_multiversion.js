/*
 * Test that replSetInitiate and replSetReconfig prohibit zero-vote arbiters,
 * SERVER-13627.
 */

load('./jstests/multiVersion/libs/multi_rs.js');

var oldVersion = '2.6';
var newVersion = 'latest';
var NewReplicaSetConfigurationIncompatible = 103;
var InvalidReplicaSetConfig = 93;

/*
 * Start 3 nodes running 'rsVersion', try to add a 0-vote arbiter at
 * 'arbiterVersion' and return the replSetReconfig response.
 */
function addZeroVoteArbiter(rsVersion, arbiterVersion) {
    var nodes = {
        n1: {binVersion: rsVersion},
        n2: {binVersion: rsVersion},
        n3: {binVersion: rsVersion}};

    var replTest = new ReplSetTest({nodes: nodes});
    var conns = replTest.startSet();
    replTest.initiate();

    var arbiterConn = replTest.add({binVersion: arbiterVersion});
    var admin = replTest.getPrimary().getDB('admin');
    var conf = replTest.conf();
    jsTestLog('current config:');
    printjson(conf);
    conf.members.push({
        _id: 3,
        host: arbiterConn.host,
        arbiterOnly: true,
        votes: 0
    });
    conf.version++;

    jsTestLog('Add arbiter with zero votes:');
    var response = admin.runCommand({replSetReconfig: conf});

    if (response.ok) {
        // Old-version primary doesn't prohibit the reconfig, but the
        // new-version arbiter removes itself when it sees it has 0 votes.
        assert.soon(function () {
            try {
                var status = replTest.status();
                printjson(status);
                return (status.members.length == 4
                    && status.members[3].state == replTest.DOWN
                    && (/configuration is invalid/i).test(status.members[3].lastHeartbeatMessage));
            } catch (exc) {
                // Not ready.
                print(exc);
                return false;
            }
        });

        // Primary stays up.
        replTest.getPrimary();
    }

    replTest.stopSet();
    return response;
}

(function newRSPlusOldArbiter() {
    jsTestLog('New-version RS with 3 nodes, adding 0-vote arbiter is prohibited.');

    var response = addZeroVoteArbiter(newVersion, oldVersion);
    assert.commandFailed(response);
    assert.eq(response.code, NewReplicaSetConfigurationIncompatible);
    assert(/.*arbiter must vote.*/i.test(response.errmsg));
})();

(function oldRSPlusNewArbiter() {
    jsTestLog('Old-version RS with 3 nodes, adding 0-vote arbiter is allowed.');

    var response = addZeroVoteArbiter(oldVersion, newVersion);

    /* Old version doesn't prohibit the reconfig. */
    assert.commandWorked(response);
})();
