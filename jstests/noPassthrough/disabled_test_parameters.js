// Test that test-only set parameters are disabled.

(function() {
    'use strict';

    function assertFails(opts) {
        assert.eq(null, MongoRunner.runMongod(opts), "Mongod startup up");
    }

    function assertStarts(opts) {
        const mongod = MongoRunner.runMongod(opts);
        assert(mongod, "Mongod startup up");
        MongoRunner.stopMongod(mongod);
    }

    setJsTestOption('enableTestCommands', false);

    // enableTestCommands not specified.
    assertFails({
        'setParameter': {
            AlwaysRecordTraffic: 'false',
        },
    });

    // enableTestCommands specified as truthy.
    ['1', 'true'].forEach(v => {
        assertStarts({
            'setParameter': {
                enableTestCommands: v,
                disableIndexSpecNamespaceGeneration: 'false',
            },
        });
    });

    // enableTestCommands specified as falsy.
    ['0', 'false'].forEach(v => {
        assertFails({
            'setParameter': {
                enableTestCommands: v,
                AlwaysRecordTraffic: 'false',
            },
        });
    });
}());
