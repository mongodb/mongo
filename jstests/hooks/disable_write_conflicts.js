(function() {
'use strict';

assert.commandWorked(
    db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));

assert.commandWorked(
    db.adminCommand({configureFailPoint: 'WTWriteConflictExceptionForReads', mode: "off"}));
})();
