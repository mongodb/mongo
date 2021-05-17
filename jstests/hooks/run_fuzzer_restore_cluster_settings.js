(function() {
'use strict';

load('jstests/libs/with_fcv.js');  // For withFCV.

withFCV(db.getMongo(), latestFCV, () => {
    // Unsetting read/write settings. This command will also cause the server to refresh and get the
    // new settings.
    assert.commandWorked(db.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {},
        defaultWriteConcern: {},
        writeConcern: {w: 1}
    }));
});
})();
