(function() {
'use strict';

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
