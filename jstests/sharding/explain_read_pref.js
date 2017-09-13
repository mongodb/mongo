/**
 * Tests read preference for explain command.
 *
 * Test is loosely based from read_pref_cmd.js.
 */

load("jstests/replsets/rslib.js");

var assertCorrectTargeting = function(explain, isMongos, secExpected) {
    assert.commandWorked(explain);

    var serverInfo;
    if (isMongos) {
        serverInfo = explain.queryPlanner.winningPlan.shards[0].serverInfo;
    } else {
        serverInfo = explain.serverInfo;
    }

    var explainDestConn = new Mongo(serverInfo.host + ':' + serverInfo.port);
    var isMaster = explainDestConn.getDB('admin').runCommand({isMaster: 1});

    if (secExpected) {
        assert(isMaster.secondary);
    } else {
        assert(isMaster.ismaster);
    }
};

var testAllModes = function(conn, isMongos) {

    // The primary is tagged with { tag: 'one' } and the secondary with
    // { tag: 'two' } so we can test the interaction of modes and tags. Test
    // a bunch of combinations.
    [
        // mode, tagSets, expectedHost
        ['primary', undefined, false],
        ['primary', [{}], false],

        ['primaryPreferred', undefined, false],
        ['primaryPreferred', [{tag: 'one'}], false],
        // Correctly uses primary and ignores the tag
        ['primaryPreferred', [{tag: 'two'}], false],

        ['secondary', undefined, true],
        ['secondary', [{tag: 'two'}], true],
        ['secondary', [{tag: 'doesntexist'}, {}], true],
        ['secondary', [{tag: 'doesntexist'}, {tag: 'two'}], true],

        ['secondaryPreferred', undefined, true],
        ['secondaryPreferred', [{tag: 'one'}], false],
        ['secondaryPreferred', [{tag: 'two'}], true],

        // We don't have a way to alter ping times so we can't predict where an
        // untagged 'nearest' command should go, hence only test with tags.
        ['nearest', [{tag: 'one'}], false],
        ['nearest', [{tag: 'two'}], true]

    ].forEach(function(args) {
        var mode = args[0], tagSets = args[1], secExpected = args[2];

        var testDB = conn.getDB('TestDB');
        conn.setSlaveOk(false);  // purely rely on readPref
        jsTest.log('Testing mode: ' + mode + ', tag sets: ' + tojson(tagSets));

        // .explain().find()
        var explainableQuery = testDB.user.explain().find();
        explainableQuery.readPref(mode, tagSets);
        var explain = explainableQuery.finish();
        assertCorrectTargeting(explain, isMongos, secExpected);

        // Set read pref on the connection.
        var oldReadPrefMode = testDB.getMongo().getReadPrefMode();
        var oldReadPrefTagSet = testDB.getMongo().getReadPrefTagSet();
        try {
            testDB.getMongo().setReadPref(mode, tagSets);

            // .explain().count();
            explain = testDB.user.explain().count();
            assertCorrectTargeting(explain, isMongos, secExpected);

            // .explain().distinct()
            explain = testDB.user.explain().distinct("_id");
            assertCorrectTargeting(explain, isMongos, secExpected);

            // .explain().group()
            explain = testDB.user.explain().group(
                {key: {_id: 1}, reduce: function(curr, result) {}, initial: {}});
            assertCorrectTargeting(explain, isMongos, secExpected);
        } finally {
            // Restore old read pref.
            testDB.getMongo().setReadPref(oldReadPrefMode, oldReadPrefTagSet);
        }
    });
};

var st = new ShardingTest({shards: {rs0: {nodes: 2}}});
st.stopBalancer();

awaitRSClientHosts(st.s, st.rs0.nodes);

// Tag primary with { dc: 'ny', tag: 'one' }, secondary with { dc: 'ny', tag: 'two' }
var primary = st.rs0.getPrimary();
var secondary = st.rs0.getSecondary();
var PRIMARY_TAG = {dc: 'ny', tag: 'one'};
var SECONDARY_TAG = {dc: 'ny', tag: 'two'};

var rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));
rsConfig.members.forEach(function(member) {
    if (member.host == primary.host) {
        member.tags = PRIMARY_TAG;
    } else {
        member.tags = SECONDARY_TAG;
    }
});

rsConfig.version++;

jsTest.log('new rsconf ' + tojson(rsConfig));

try {
    primary.adminCommand({replSetReconfig: rsConfig});
} catch (e) {
    jsTest.log('replSetReconfig error: ' + e);
}

st.rs0.awaitSecondaryNodes();

// Force mongos to reconnect after our reconfig and also create the test database
assert.soon(function() {
    try {
        st.s.getDB('TestDB').runCommand({create: 'TestColl'});
        return true;
    } catch (x) {
        // Intentionally caused an error that forces mongos's monitor to refresh.
        jsTest.log('Caught exception while doing dummy command: ' + tojson(x));
        return false;
    }
});

reconnect(primary);
reconnect(secondary);

rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));

var replConn = new Mongo(st.rs0.getURL());

// Make sure replica set connection is ready
_awaitRSHostViaRSMonitor(primary.name, {ok: true, tags: PRIMARY_TAG}, st.rs0.name);
_awaitRSHostViaRSMonitor(secondary.name, {ok: true, tags: SECONDARY_TAG}, st.rs0.name);

testAllModes(replConn, false);

jsTest.log('Starting test for mongos connection');

testAllModes(st.s, true);

st.stop();
