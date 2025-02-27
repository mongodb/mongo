/**
 * Tests that the server is able to restart in read-only mode with data files that contain one or
 * more temporary collections. The temporary collection will be dropped during startup recovery.
 *
 * @tags: [requires_replication]
 */
'use strict';
load('jstests/readonly/lib/read_only_test.js');

runReadOnlyTest((function() {
    return {
        name: 'temp_collection',

        load: function(collection) {
            let collName = collection.getName();
            let db = collection.getDB();
            db[collName].drop();

            assert.commandWorked(db.runCommand({
                applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {create: collName, temp: true}}]
            }));

            let collectionInfos = db.getCollectionInfos();
            let collectionExists = false;
            collectionInfos.forEach(info => {
                if (info.name === collName) {
                    assert(info.options.temp,
                           'The collection is not marked as a temporary one\n' +
                               tojson(collectionInfos));
                    collectionExists = true;
                }
            });
            assert(collectionExists, 'Can not find collection in collectionInfos');
            assert.commandWorked(collection.insert({a: 1}));
        },

        exec: function(collection) {
            // Temporary collections are dropped during startup recovery.
            let collName = collection.getName();
            let db = collection.getDB();

            let collectionInfos = db.getCollectionInfos();
            let collectionExists = false;
            collectionInfos.forEach(info => {
                if (info.name === collName) {
                    collectionExists = true;
                }
            });

            assert(!collectionExists);
        }
    };
})());
