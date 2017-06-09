/**
 * This test will ensure that a failed a batch apply will become consistent only when passing
 * the end boundary (minvalid) in subsequent applies.
 *
 * To do this we:
 * -- Set minvalid manually on primary (node0) way ahead (5 minutes)
 * -- Restart primary (node0)
 * -- Ensure restarted primary (node0) comes up in recovering
 * -- Ensure node0 blacklists new primary as a sync source and keeps the old minvalid
 * -- Success!
 *
 * This test requires persistence to test that a restarted primary will stay in the RECOVERING state
 * when minvalid is set to the future. An ephemeral storage engine will not have a minvalid after
 * restarting, so will initial sync in this scenario, invalidating the test.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    function tsToDate(ts) {
        return new Date(ts.getTime() * 1000);
    }

    var replTest =
        new ReplSetTest({name: "apply_batch_only_goes_forward", nodes: [{}, {}, {arbiter: true}]});

    var nodes = replTest.startSet();
    replTest.initiate();
    var master = replTest.getPrimary();
    var mTest = master.getDB("test");
    var mLocal = master.getDB("local");
    var mMinvalid = mLocal["replset.minvalid"];

    var slave = replTest.getSecondary();
    var sTest = slave.getDB("test");
    var sLocal = slave.getDB("local");
    var sMinvalid = sLocal["replset.minvalid"];
    var stepDownSecs = 30;
    var stepDownCmd = {replSetStepDown: stepDownSecs, force: true};

    // Write op
    assert.writeOK(
        mTest.foo.save({}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
    replTest.waitForState(slave, ReplSetTest.State.SECONDARY);
    assert.writeOK(
        mTest.foo.save({}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

    // Set minvalid to something far in the future for the current primary, to simulate recovery.
    // Note: This is so far in the future (5 days) that it will never become secondary.
    var farFutureTS = new Timestamp(
        Math.floor(new Date().getTime() / 1000) + (60 * 60 * 24 * 5 /* in five days*/), 0);
    var rsgs = assert.commandWorked(mLocal.adminCommand("replSetGetStatus"));
    var primaryOpTime = rsgs.members
                            .filter(function(member) {
                                return member.self;
                            })[0]
                            .optime;
    jsTest.log("future TS: " + tojson(farFutureTS) + ", date:" + tsToDate(farFutureTS));
    // We do an update in case there is a minvalid document on the primary already.
    // If the doc doesn't exist then upsert:true will create it, and the writeConcern ensures
    // that update returns details of the write, like whether an update or insert was performed.
    printjson(assert.writeOK(mMinvalid.update(
        {},
        {ts: farFutureTS, t: NumberLong(-1), begin: primaryOpTime},
        {upsert: true, writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}})));

    jsTest.log('Restarting primary ' + master.host +
               ' with updated minValid. This node will go into RECOVERING upon restart. ' +
               'Secondary ' + slave.host + ' will become new primary.');
    clearRawMongoProgramOutput();
    replTest.restart(master);
    printjson(sLocal.adminCommand("isMaster"));
    replTest.waitForState(master, ReplSetTest.State.RECOVERING);

    // Slave is now master... Do a write to advance the optime on the primary so that it will be
    // considered as a sync source -  this is more relevant to PV0 because we do not write a new
    // entry to the oplog on becoming primary.
    assert.writeOK(replTest.getPrimary().getDB("test").foo.save(
        {}, {writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

    // Sync source selection will log this message if it does not detect min valid in the sync
    // source candidate's oplog.
    assert.soon(function() {
        return rawMongoProgramOutput().match(
            'it does not contain the necessary operations for us to reach a consistent state');
    });

    assert.soon(function() {
        var mv;
        try {
            mv = mMinvalid.findOne();
        } catch (e) {
            return false;
        }
        var msg = "ts !=, " + farFutureTS + "(" + tsToDate(farFutureTS) + "), mv:" + tojson(mv) +
            " - " + tsToDate(mv.ts);
        assert.eq(farFutureTS, mv.ts, msg);
        return true;
    });

    // Shut down the set and finish the test.
    replTest.stopSet();
})();
