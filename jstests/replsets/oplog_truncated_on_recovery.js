/**
 * This test will ensure that a failed a batch apply will remove the any oplog
 * entries from that batch.
 *
 * To do this we: -- Create single node replica set -- Set minvalid manually on
 * primary way ahead (5 minutes) -- Write some oplog entries newer than
 * minvalid.start -- Ensure restarted primary comes up in recovering and
 * truncates the oplog -- Success!
 */
(function() {
    "use strict";

    function tsToDate(ts) {
        return new Date(ts.getTime() * 1000);
    }

    function log(arg) {
        jsTest.log(tojson(arg));
    }

    var replTest = new ReplSetTest(
        {
            name : "oplog_truncated_on_recovery",
            nodes : 1
        });

    var nodes = replTest.startSet();
    replTest.initiate();
    var master = replTest.getMaster();
    var testDB = master.getDB("test");
    var localDB = master.getDB("local");
    var minvalidColl = localDB["replset.minvalid"];

    // Write op
    log(assert.writeOK(testDB.foo.save(
        {
            _id : 1,
            a : 1
        },
        {
            writeConcern :
                {
                    w : 1
                }
        })));

    // Set minvalid to something far in the future for the current primary, to
    // simulate recovery.
    // Note: This is so far in the future (5 days) that it will never become
    // secondary.
    var farFutureTS = new Timestamp(Math.floor(new Date().getTime() / 1000)
                                    + (60 * 60 * 24 * 5 /* in five days */), 0);
    var rsgs = assert.commandWorked(localDB.adminCommand("replSetGetStatus"));
    log(rsgs);
    var primaryOpTime = rsgs.members[0].optime;
    var primaryLastTS = rsgs.members[0].optime.ts;
    log(primaryLastTS);

    // Set the start of the failed batch
    primaryOpTime.ts = new Timestamp(primaryOpTime.ts.t, primaryOpTime.ts.i + 1);

    log(primaryLastTS);
    jsTest.log("future TS: " + tojson(farFutureTS) + ", date:" + tsToDate(farFutureTS));
    // We do an update in case there is a minvalid document on the primary
    // already.
    // If the doc doesn't exist then upsert:true will create it, and the
    // writeConcern ensures
    // that update returns details of the write, like whether an update or
    // insert was performed.
    log(assert.writeOK(minvalidColl.update(
        {},
        {
            ts : farFutureTS,
            t : NumberLong(-1),
            begin : primaryOpTime
        },
        {
            upsert : true,
            writeConcern :
                {
                    w : 1
                }
        })));

    log(assert.writeOK(localDB.oplog.rs.insert(
        {
            _id : 0,
            ts : primaryOpTime.ts,
            op : "n",
            term : -1
        })));
    log(localDB.oplog.rs.find().toArray());
    log(assert.commandWorked(localDB.adminCommand("replSetGetStatus")));
    log("restart primary");
    replTest.restart(master);
    replTest.waitForState(master, replTest.RECOVERING, 90000);

    assert.soon(function() {
        var mv;
        try {
            mv = minvalidColl.findOne();
        }
        catch (e) {
            return false;
        }
        var msg = "ts !=, " + farFutureTS + "(" + tsToDate(farFutureTS) + "), mv:" + tojson(mv)
                  + " - " + tsToDate(mv.ts);
        assert.eq(farFutureTS, mv.ts, msg);

        var lastTS = localDB.oplog.rs.find().sort(
            {
                $natural : -1
            }).limit(-1).next().ts;
        log(localDB.oplog.rs.find().toArray());
        assert.eq(primaryLastTS, lastTS);
        return true;
    });

    // Shut down the set and finish the test.
    replTest.stopSet();
})();