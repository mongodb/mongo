(function() {
'use strict';

// Ignore "attempted to set unrecognized parameter" error from setParameter on old versions
let res;
try {
    res = db.adminCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"});
} catch (e) {
    print("Failed to set requireApiVersion.");
    print(e);
}
if (res && res.ok === 0 && !res.errmsg.includes("attempted to set unrecognized parameter")) {
    assert.commandWorked(res);
}

// Unsetting read/write settings. This command will also cause the server to refresh and get
// the new settings. A standalone, mongos or old version will return an error; ignore it.
const result = db.adminCommand({
    setDefaultRWConcern: 1,
    defaultReadConcern: {},
    defaultWriteConcern: {},
    writeConcern: {w: 1}
});
assert.commandWorkedOrFailedWithCode(result, [51300, 51301, 40415]);
})();
