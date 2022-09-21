/**
 * Verify that mirroredReads that pass the slow query threshold set mirrored:true.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_61,
 * ]
 */

(function() {
"use strict";

function setParameter({rst, value}) {
    return rst.getPrimary().adminCommand({setParameter: 1, mirrorReads: value});
}

const kDbName = "test";
const kCollName = jsTestName();

function verifySlowLogsIndicateMirroredReads(rst, cmd) {
    jsTestLog("Verifying log message indicates mirrored operation for cmd '" + tojson(cmd));
    let primaryAdminDb = rst.getPrimary().getDB('admin');
    let secondaryAdminDb = rst.getSecondary().getDB('admin');

    rst.getPrimary().getDB(kDbName).runCommand(cmd);

    const logId = 51803;
    // Check that the primary slow log messages do not print with mirrored: true.
    assert(checkLog.checkContainsWithCountJson(primaryAdminDb,
                                               logId,
                                               {mirrored: true},
                                               /*expectedCount=*/0,
                                               /*severity=*/null,
                                               /*isRelaxed=*/true));
    assert.soon(() => {
        // Check that the secondary slow log message is printed with mirrored: true.
        return checkLog.checkContainsWithCountJson(
            secondaryAdminDb,
            logId,
            {mirrored: true, ns: kDbName.concat(".", kCollName)},
            /*expectedCount=*/1,
            /*severity=*/null,
            /*isRelaxed=*/true);
    });
    assert.commandWorked(secondaryAdminDb.adminCommand({clearLog: "global"}));
}

{
    const rst = new ReplSetTest({
        nodes: 2,
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    // Put in a datum
    {
        let ret =
            rst.getPrimary().getDB(kDbName).runCommand({insert: kCollName, documents: [{x: 1}]});
        assert.commandWorked(ret);
    }

    // Set the slow query logging threshold (slowMS) to -1 to ensure every query gets logged on the
    // secondary.
    rst.getSecondary().getDB('admin').setProfilingLevel(0, -1);

    // Have every operation be mirrored.
    assert.commandWorked(setParameter({rst: rst, value: {samplingRate: 1.0}}));

    verifySlowLogsIndicateMirroredReads(rst, {find: kCollName, filter: {}});
    verifySlowLogsIndicateMirroredReads(rst, {count: kCollName, query: {}});
    verifySlowLogsIndicateMirroredReads(rst, {distinct: kCollName, key: "x"});
    verifySlowLogsIndicateMirroredReads(
        rst, {findAndModify: kCollName, query: {}, update: {'$inc': {x: 1}}});
    verifySlowLogsIndicateMirroredReads(
        rst, {update: kCollName, updates: [{q: {_id: 1}, u: {'$inc': {x: 1}}}], ordered: false});

    rst.stopSet();
}
})();
