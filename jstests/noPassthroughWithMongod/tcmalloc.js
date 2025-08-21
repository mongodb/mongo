import {findMatchingLogLine} from "jstests/libs/log.js";
// Assert setting tcmalloc_release_rate with setParameter.

// Check that setParameter is available on this build. And whether tcmallocReleaseRate is.
function hasTcSetParameter() {
    const commandResult = db.adminCommand({getParameter: 1, tcmallocReleaseRate: 1});
    if (commandResult.ok) return true;
    else return false;
}

function hasTCMallocSmallValueLogLine() {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    return findMatchingLogLine(globalLog.log, {id: 8627602}) !== null;
}

if (hasTcSetParameter()) {
    assert.commandWorked(db.adminCommand({clearLog: "global"}));
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 0}));
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 101}));
    assert(!hasTCMallocSmallValueLogLine());

    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 1}));
    assert(hasTCMallocSmallValueLogLine());
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 10}));
    assert(hasTCMallocSmallValueLogLine());
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 100}));
    assert(hasTCMallocSmallValueLogLine());
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 5.0}));
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 0.01}));
    assert.commandFailed(db.adminCommand({setParameter: 1, tcmallocReleaseRate: -1.0}));
    assert.commandFailed(db.adminCommand({setParameter: 1, tcmallocReleaseRate: "foo"}));
}
