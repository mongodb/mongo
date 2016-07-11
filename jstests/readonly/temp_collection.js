// Tests that the server is able to restart in read-only mode with data files that contain one or
// more temporary collections. See SERVER-24719 for details.

'use strict';
load('jstests/readonly/lib/read_only_test.js');

runReadOnlyTest((function() {
    return {
        name: 'temp_collection',

        load: function(writableCollection) {
            var collName = writableCollection.getName();
            var writableDB = writableCollection.getDB();
            writableDB[collName].drop();

            assert.commandWorked(writableDB.createCollection(collName, {temp: true}));

            var collectionInfos = writableDB.getCollectionInfos();
            var collectionExists = false;
            collectionInfos.forEach(info => {
                if (info.name === collName) {
                    assert(info.options.temp,
                           'The collection is not marked as a temporary one\n' +
                               tojson(collectionInfos));
                    collectionExists = true;
                }
            });
            assert(collectionExists, 'Can not find collection in collectionInfos');
            assert.writeOK(writableCollection.insert({a: 1}));
        },

        exec: function(readableCollection) {
            assert.eq(
                readableCollection.count(), 1, 'There should be 1 document in the temp collection');
        }
    };
})());
