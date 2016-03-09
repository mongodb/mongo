load('jstests/readonly/lib/read_only_test.js');

runReadOnlyTest(function() {
    'use strict';

    return {
        name: 'server_status',

        load: function(writableCollection) {},
        exec: function(readableCollection) {
            assert.commandWorked(readableCollection.getDB().serverStatus());
        }
    };
}());
