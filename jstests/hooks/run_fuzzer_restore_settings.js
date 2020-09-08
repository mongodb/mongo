(function() {
'use strict';

assert.commandWorked(db.adminCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));

// Unsetting read/write settings. This command will also cause the server to refresh and get
// the new settings. A standalone or mongos will return an error; ignore it.
const result = db.adminCommand({
    setDefaultRWConcern: 1,
    defaultReadConcern: {},
    defaultWriteConcern: {},
    writeConcern: {w: 1}
});
assert.commandWorkedOrFailedWithCode(result, [51300, 51301]);
})();
