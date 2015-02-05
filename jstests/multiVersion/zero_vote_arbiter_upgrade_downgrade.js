/*
 * Test zero-vote arbiters during upgrade and downgrade between 2.6 and 2.8.
 * SERVER-13627.
 */

load('./jstests/multiVersion/libs/multi_rs.js');

var oldVersion = '2.6';
var newVersion = 'latest';

/*
 * Create a 2.6 replica set with a 0-vote arbiter and upgrade it to the latest
 * version. Check that we still have a quorum and can elect a primary. Downgrade
 * it, check again.
 */
(function upgradeAndDowngradeRSWithAZeroVoteArbiter() {
    var nodes = {
        n1: {binVersion: oldVersion},
        n2: {binVersion: oldVersion},
        a3: {binVersion: oldVersion}};

    var replTest = new ReplSetTest({nodes: nodes});
    replTest.startSet();
    var conf = replTest.getReplSetConfig();
    conf.members[2].votes = 0;
    replTest.initiate(conf);

    jsTestLog('Upgrade to ' + newVersion);
    replTest.upgradeSet(newVersion);

    // Should be able to elect a primary.
    replTest.getPrimary();

    jsTestLog('Config after upgrade:');
    printjson(replTest.conf());

    jsTestLog('Downgrade to ' + oldVersion);
    replTest.upgradeSet(oldVersion);

    // Should be able to elect a primary.
    replTest.getPrimary();
    jsTestLog('Config after downgrade:');
    printjson(replTest.conf());

    replTest.stopSet();
})();
