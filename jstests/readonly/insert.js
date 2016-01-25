load("jstests/readonly/lib/read_only_test.js");

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'insert',
        load: function(writableCollection) {
            assert.writeOK(writableCollection.insert({x: 1}));
        },
        exec: function(readableCollection) {
            assert.writeError(readableCollection.insert({x: 2}));
        }
    };
}());
