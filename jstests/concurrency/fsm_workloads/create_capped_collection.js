'use strict';

/**
 * create_capped_collection.js
 *
 * Repeatedly creates a capped collection. Also verifies that truncation
 * occurs once the collection reaches a certain size.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');    // for dropCollections
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongod and isMMAPv1

var $config = (function() {

    // Returns a document of the form { _id: ObjectId(...), field: '...' }
    // with specified BSON size.
    function makeDocWithSize(targetSize) {
        var doc = {_id: new ObjectId(), field: ''};

        var size = Object.bsonsize(doc);
        assertAlways.gte(targetSize, size);

        // Set 'field' as a string with enough characters
        // to make the whole document 'size' bytes long
        doc.field = new Array(targetSize - size + 1).join('x');
        assertAlways.eq(targetSize, Object.bsonsize(doc));

        return doc;
    }

    // Inserts a document of a certain size into the specified collection
    // and returns its _id field.
    function insert(db, collName, targetSize) {
        var doc = makeDocWithSize(targetSize);

        var res = db[collName].insert(doc);
        assertAlways.writeOK(res);
        assertAlways.eq(1, res.nInserted);

        return doc._id;
    }

    // Returns an array containing the _id fields of all the documents
    // in the collection, sorted according to their insertion order.
    function getObjectIds(db, collName) {
        return db[collName].find({}, {_id: 1}).map(function(doc) {
            return doc._id;
        });
    }

    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'create_capped_collection',
        insert: insert,
        getObjectIds: getObjectIds,

        // Define this function in data so that it can be used by workloads inheriting this one
        verifySizeTruncation: function verifySizeTruncation(db, myCollName, options) {
            var ids = [];
            var count;

            // Define a small document to be an eighth the size of the capped collection,
            // and a large document to be half the size of the capped collection.
            var smallDocSize = Math.floor(options.size / 8) - 1;
            var largeDocSize = Math.floor(options.size / 2) - 1;

            // Truncation in MMAPv1 has well defined behavior.
            if (isMongod(db) && isMMAPv1(db)) {
                ids.push(this.insert(db, myCollName, largeDocSize));

                // Insert a large document and verify that a truncation has occurred.
                // There should be 1 document in the collection and it should always be
                // the most recently inserted document.

                ids.push(this.insert(db, myCollName, largeDocSize));

                count = db[myCollName].find().itcount();
                assertWhenOwnDB.eq(count, 1, 'expected truncation to occur');
                assertWhenOwnDB.eq(ids.slice(ids.length - count),
                                   this.getObjectIds(db, myCollName),
                                   'expected truncation to remove the oldest document');

                // Insert multiple small documents and verify that truncation has occurred. There
                // should be at most 4 documents in the collection (fewer based on the maximum
                // number of documents allowed if specified during collection creation), and they
                // should be the most recently inserted documents.

                ids.push(this.insert(db, myCollName, smallDocSize));
                ids.push(this.insert(db, myCollName, smallDocSize));
                ids.push(this.insert(db, myCollName, smallDocSize));

                var prevCount = count;
                count = db[myCollName].find().itcount();

                var expectedCount = options.max && options.max < 4 ? options.max : 4;

                assertWhenOwnDB.eq(count, expectedCount, 'expected truncation to occur');
                assertWhenOwnDB.eq(ids.slice(ids.length - count),
                                   this.getObjectIds(db, myCollName),
                                   'expected truncation to remove the oldest documents');
            }

            // Truncation of capped collections is generally unreliable. Instead of relying on it
            // to occur after a certain size document is inserted we test its occurrence. We set a
            // reasonable threshold of documents to insert before a user might expect truncation to
            // occur and verify truncation occurred for the right documents.

            var threshold = 1000;

            for (var i = 0; i < threshold; ++i) {
                ids.push(this.insert(db, myCollName, largeDocSize));
            }

            count = db[myCollName].find().itcount();

            assertWhenOwnDB.lt(count, threshold, 'expected at least one truncation to occur');
            assertWhenOwnDB.eq(ids.slice(ids.length - count),
                               this.getObjectIds(db, myCollName),
                               'expected truncation to remove the oldest documents');
        }
    };

    var states = (function() {

        var options = {
            capped: true,
            size: 8192  // multiple of 256; larger than 4096 default
        };

        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        // TODO: how to avoid having too many files open?
        function create(db, collName) {
            var myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
            assertAlways.commandWorked(db.createCollection(myCollName, options));

            this.verifySizeTruncation(db, myCollName, options);
        }

        return {init: init, create: create};

    })();

    var transitions = {init: {create: 1}, create: {create: 1}};

    function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '\\d+_\\d+$');
        dropCollections(db, pattern);
    }

    return {
        threadCount: 5,
        iterations: 5,
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown
    };

})();
