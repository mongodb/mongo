// Ensures db eval does not crash when attempt is made to wait on a replicated read/write concern
(function() {
    'use strict';

    // Read concern
    var findCommand = {find: 'readMajority', batchSize: 2, readConcern: {level: 'majority'}};
    db.runCommand({'eval': 'db.runCommand(' + tojson(findCommand) + ')'});

    // Write concern
    var insertCommand = {
        insert: 'writeConcern',
        documents: [{TestKey: 'TestValue'}],
        writeConcern: {w: 'majority', wtimeout: 30000}
    };
    db.runCommand({'eval': 'db.runCommand(' + tojson(insertCommand) + ')'});
});
