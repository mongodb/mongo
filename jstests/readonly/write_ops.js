load("jstests/readonly/lib/read_only_test.js");

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'write_ops',
        load: function(writableCollection) {
            assert.writeOK(writableCollection.insert({x: 1}));
        },
        exec: function(readableCollection) {
            // Test that insert fails.
            assert.writeError(readableCollection.insert({x: 2}));

            // Test that delete fails.
            assert.writeError(readableCollection.remove({x: 1}));

            // Test that update fails.
            assert.writeError(readableCollection.update({x: 1}, {$inc: {x: 1}}));
        }
    };
}());
