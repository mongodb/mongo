load("jstests/readonly/lib/read_only_test.js");
/*
 * TODO SERVER-45483: The write command should only be returning an
 * IlegalOperation
 */
runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'write_ops',
        load: function(writableCollection) {
            assert.writeOK(writableCollection.insert({_id: 0, x: 1}));
        },
        exec: function(readableCollection) {
            // Test that insert fails.
            assert.writeErrorWithCode(
                readableCollection.insert({x: 2}),
                [ErrorCodes.IllegalOperation, ErrorCodes.MultipleErrorsOccurred],
                "Expected insert to fail because database is in read-only mode");

            // Test that delete fails.
            assert.writeErrorWithCode(
                readableCollection.remove({x: 1}),
                [ErrorCodes.IllegalOperation, ErrorCodes.MultipleErrorsOccurred],
                "Expected remove to fail because database is in read-only mode");

            // Test that update fails.
            assert.writeErrorWithCode(
                readableCollection.update({_id: 0}, {$inc: {x: 1}}),
                [ErrorCodes.IllegalOperation, ErrorCodes.MultipleErrorsOccurred],
                "Expected update to fail because database is in read-only mode");
        }
    };
}());
