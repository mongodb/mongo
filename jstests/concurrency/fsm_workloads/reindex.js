'use strict';

/**
 * reindex.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between reindexing and querying
 * against the collection. Operates on a separate collection for each thread.
 */

load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = (function() {
    var data = {
        nIndexes: 3 + 1,  // 3 created and 1 for _id
        nDocumentsToInsert: 1000,
        maxInteger: 100,   // Used for document values. Must be a factor of nDocumentsToInsert
        prefix: 'reindex'  // Use filename for prefix because filename is assumed unique
    };

    var states = (function() {
        function insertDocuments(db, collName) {
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.nDocumentsToInsert; ++i) {
                bulk.insert({
                    text: 'Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do' +
                        ' eiusmod tempor incididunt ut labore et dolore magna aliqua.',
                    geo: {type: 'Point', coordinates: [(i % 50) - 25, (i % 50) - 25]},
                    integer: i % this.maxInteger
                });
            }
            var res = bulk.execute();
            assertAlways.writeOK(res);
            assertAlways.eq(this.nDocumentsToInsert, res.nInserted);
        }

        function createIndexes(db, collName) {
            // The number of indexes created here is also stored in data.nIndexes
            var textResult = db[this.threadCollName].ensureIndex({text: 'text'});
            assertAlways.commandWorked(textResult);

            var geoResult = db[this.threadCollName].ensureIndex({geo: '2dsphere'});
            assertAlways.commandWorked(geoResult);

            var integerResult = db[this.threadCollName].ensureIndex({integer: 1});
            assertAlways.commandWorked(integerResult);
        }

        function init(db, collName) {
            this.threadCollName = this.prefix + '_' + this.tid;
            insertDocuments.call(this, db, this.threadCollName);
        }

        function query(db, collName) {
            var coll = db[this.threadCollName];
            var nInsertedDocuments = this.nDocumentsToInsert;
            var count = coll.find({integer: Random.randInt(this.maxInteger)}).itcount();
            assertWhenOwnColl.eq(
                nInsertedDocuments / this.maxInteger,
                count,
                'number of ' +
                    'documents returned by integer query should match the number ' +
                    'inserted');

            var coords = [[[-26, -26], [-26, 26], [26, 26], [26, -26], [-26, -26]]];
            var geoQuery = {geo: {$geoWithin: {$geometry: {type: 'Polygon', coordinates: coords}}}};

            // We can only perform a geo query when we own the collection and are sure a geo index
            // is present. The same is true of text queries.
            assertWhenOwnColl(function() {
                count = coll.find(geoQuery).itcount();
                assertWhenOwnColl.eq(count,
                                     nInsertedDocuments,
                                     'number of documents returned by' +
                                         ' geospatial query should match number inserted');

                count = coll.find({$text: {$search: 'ipsum'}}).itcount();
                assertWhenOwnColl.eq(count,
                                     nInsertedDocuments,
                                     'number of documents returned by' +
                                         ' text query should match number inserted');
            });

            var indexCount = db[this.threadCollName].getIndexes().length;
            assertWhenOwnColl.eq(indexCount, this.nIndexes);
        }

        function reIndex(db, collName) {
            var res = db[this.threadCollName].reIndex();
            assertAlways.commandWorked(res);
        }

        return {init: init, createIndexes: createIndexes, reIndex: reIndex, query: query};
    })();

    var transitions = {
        init: {createIndexes: 1},
        createIndexes: {reIndex: 0.5, query: 0.5},
        reIndex: {reIndex: 0.5, query: 0.5},
        query: {reIndex: 0.5, query: 0.5}
    };

    var teardown = function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '_\\d+$');
        dropCollections(db, pattern);
    };

    return {
        threadCount: 15,
        iterations: 10,
        states: states,
        transitions: transitions,
        teardown: teardown,
        data: data
    };
})();
