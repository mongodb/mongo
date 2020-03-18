(function() {
'use strict';

// Unsetting read/write settings. This command will also cause the server to refresh and get
// the new settings.
assert.commandWorked(
    db.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {}, defaultWriteConcern: {}}));
})();
