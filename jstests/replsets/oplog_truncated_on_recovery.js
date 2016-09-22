/**
 * This test will ensure that recovery from a failed batch application will remove the oplog
 * entries from that batch.
 *
 * To do this we:
 * -- Create single node replica set
 * -- Set minvalid manually on primary way ahead (5 days)
 * -- Write some oplog entries newer than minvalid.start
 * -- Ensure restarted primary comes up in recovering and truncates the oplog
 * -- Success!
 *
 * This test requires persistence for two reasons:
 *  1. To test that a restarted primary will stay in the RECOVERING state when minvalid is set to
 *     the future. An ephemeral storage engine will not have a minvalid after a restart, so the node
 *     will start an initial sync in this scenario, invalidating the test.
 *  2. It uses a single node replica set, which cannot be restarted in any meaningful way with an
 *     ephemeral storage engine.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    function tsToDate(ts) {
        return new Date(ts.getTime() * 1000);
    }

    function log(arg) {
        jsTest.log(tojson(arg));
    }

    var replTest = new ReplSetTest({name: "oplog_truncated_on_recovery", nodes: 1});

    var nodes = replTest.startSet();
    replTest.initiate();
    var master = replTest.getPrimary();
    var testDB = master.getDB("test");
    var localDB = master.getDB("local");
    var minvalidColl = localDB["replset.minvalid"];

    // Write op
    log(assert.writeOK(testDB.foo.save({_id: 1, a: 1}, {writeConcern: {w: 1}})));

    // Set minvalid to something far in the future for the current primary, to simulate recovery.
    // Note: This is so far in the future (5 days) that it will never become secondary.
    var farFutureTS = new Timestamp(
        Math.floor(new Date().getTime() / 1000) + (60 * 60 * 24 * 5 /* in five days */), 0);
    var rsgs = assert.commandWorked(localDB.adminCommand("replSetGetStatus"));
    log(rsgs);
    var primaryOpTime = rsgs.members[0].optime;
    log(primaryOpTime);

    // Set the start of the failed batch
    // TODO this test should restart in stand-alone mode to futz with the state rather than trying
    // to do it on a running primary.

    jsTest.log("future TS: " + tojson(farFutureTS) + ", date:" + tsToDate(farFutureTS));
    var divergedTS = new Timestamp(primaryOpTime.ts.t, primaryOpTime.ts.i + 1);
    // We do an update in case there is a minvalid document on the primary already.
    // If the doc doesn't exist then upsert:true will create it, and the writeConcern ensures
    // that update returns details of the write, like whether an update or insert was performed.
    log(assert.writeOK(minvalidColl.update({},
                                           {
                                             ts: farFutureTS,
                                             t: NumberLong(-1),
                                             begin: primaryOpTime,
                                             oplogDeleteFromPoint: divergedTS
                                           },
                                           {upsert: true, writeConcern: {w: 1}})));

    // Insert a diverged oplog entry that will be truncated after restart.
    log(assert.writeOK(localDB.oplog.rs.insert(
        {_id: 0, ts: divergedTS, op: "n", h: NumberLong(0), t: NumberLong(-1)})));
    log(localDB.oplog.rs.find().toArray());
    log(assert.commandWorked(localDB.adminCommand("replSetGetStatus")));
    log("restart primary");
    replTest.restart(master);
    replTest.waitForState(master, ReplSetTest.State.RECOVERING);

    assert.soon(function() {
        var mv;
        try {
            mv = minvalidColl.findOne();
        } catch (e) {
            return false;
        }
        var msg = "ts !=, " + farFutureTS + "(" + tsToDate(farFutureTS) + "), mv:" + tojson(mv) +
            " - " + tsToDate(mv.ts);
        assert.eq(farFutureTS, mv.ts, msg);

        var lastTS = localDB.oplog.rs.find().sort({$natural: -1}).limit(-1).next().ts;
        log(localDB.oplog.rs.find().toArray());
        assert.eq(primaryOpTime.ts, lastTS);
        return true;
    });

    // Shut down the set and finish the test.
    replTest.stopSet();
})();
