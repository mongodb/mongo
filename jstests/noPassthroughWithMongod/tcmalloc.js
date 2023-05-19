// Assert setting tcmalloc_release_rate with setParameter.

(function() {
"use strict";

// Check that setParameter is available on this build. And whether tcmallocReleaseRate is.
function hasTcSetParameter() {
    const commandResult = db.adminCommand({getParameter: 1, tcmallocReleaseRate: 1});
    if (commandResult.ok)
        return true;
    else
        return false;
}

if (hasTcSetParameter()) {
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 10}));
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 5.0}));
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 0.01}));
    assert.commandWorked(db.adminCommand({setParameter: 1, tcmallocReleaseRate: 0}));
    assert.commandFailed(db.adminCommand({setParameter: 1, tcmallocReleaseRate: -1.0}));
    assert.commandFailed(db.adminCommand({setParameter: 1, tcmallocReleaseRate: "foo"}));
}
}());
