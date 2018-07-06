// SERVER-35112: Test that specifying removed MMAPv1 specific options gives correct error.
(function() {
    'use strict';
    db.repair_unsupported_options.drop();
    assert.commandWorked(db.repair_unsupported_options.insert({}));  // Ensure database exists.
    let badValue = (cmd) => assert.commandFailedWithCode(db.runCommand(cmd), ErrorCodes.BadValue);
    badValue({repairDatabase: 1, preserveClonedFilesOnFailure: 1});
    badValue({repairDatabase: 1, backupOriginalFiles: 1});
    assert.commandWorked(db.runCommand({repairDatabase: 1, someRandomUnknownOption: 1}));
})();
