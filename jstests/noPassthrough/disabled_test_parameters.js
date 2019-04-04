// Test that test-only set parameters are disabled.

(function() {
    'use strict';

    function assertFails(opts) {
        assert.eq(null, MongoRunner.runMongod(opts), "Mongod startup up");
    }

    function assertStarts(opts) {
        const merizod = MongoRunner.runMongod(opts);
        assert(merizod, "Mongod startup up");
        MongoRunner.stopMongod(merizod);
    }

    setJsTestOption('enableTestCommands', false);

    // enableTestCommands not specified.
    assertFails({
        'setParameter': {
            enableIndexBuildsCoordinatorForCreateIndexesCommand: 'false',
        },
    });

    // enableTestCommands specified as truthy.
    ['1', 'true'].forEach(v => {
        assertStarts({
            'setParameter': {
                enableTestCommands: v,
                enableIndexBuildsCoordinatorForCreateIndexesCommand: 'false',
            },
        });
    });

    // enableTestCommands specified as falsy.
    ['0', 'false'].forEach(v => {
        assertFails({
            'setParameter': {
                enableTestCommands: v,
                enableIndexBuildsCoordinatorForCreateIndexesCommand: 'false',
            },
        });
    });
}());
