(function() {
    'use strict';

    load("jstests/replsets/libs/apply_ops_insert_write_conflict.js");

    new ApplyOpsInsertWriteConflictTest({
        testName: 'apply_ops_insert_write_conflict_nonatomic',
        atomic: false
    }).run();
}());
