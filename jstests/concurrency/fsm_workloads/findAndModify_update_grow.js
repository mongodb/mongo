'use strict';

/**
 * findAndModify_update_grow.js
 *
 * Each thread inserts a single document into a collection, and then
 * repeatedly performs the findAndModify command. Attempts to trigger
 * a document move by growing the size of the inserted document using
 * the $set and $mul update operators.
 */
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongod and isMMAPv1

var $config = (function() {

    var data = {
        shardKey: {tid: 1},
    };

    var states = (function() {

        // Use the workload name as the field name (since it is assumed
        // to be unique) to avoid any potential issues with large keys
        // and indexes on the collection.
        var uniqueFieldName = 'findAndModify_update_grow';

        function makeStringOfLength(length) {
            return new Array(length + 1).join('x');
        }

        function makeDoc(tid) {
            // Use 32-bit integer for representing 'length' property
            // to ensure $mul does integer multiplication
            var doc = {_id: new ObjectId(), tid: tid, length: new NumberInt(1)};
            doc[uniqueFieldName] = makeStringOfLength(doc.length);
            return doc;
        }

        function insert(db, collName) {
            var doc = makeDoc(this.tid);
            this.length = doc.length;
            this.bsonsize = Object.bsonsize(doc);

            var res = db[collName].insert(doc);
            assertAlways.writeOK(res);
            assertAlways.eq(1, res.nInserted);
        }

        function findAndModify(db, collName) {
            // When the size of the document starts to near the 16MB limit,
            // start over with a new document
            if (this.bsonsize > 4 * 1024 * 1024 /* 4MB */) {
                insert.call(this, db, collName);
            }

            // Get the DiskLoc of the document before its potential move
            var before = db[collName]
                             .find({tid: this.tid})
                             .showDiskLoc()
                             .sort({length: 1})  // fetch document of smallest size
                             .limit(1)
                             .next();

            // Increase the length of the 'findAndModify_update_grow' string
            // to double the size of the overall document
            var factor = Math.ceil(2 * this.bsonsize / this.length);
            var updatedLength = factor * this.length;
            var updatedValue = makeStringOfLength(updatedLength);

            var update = {$set: {}, $mul: {length: factor}};
            update.$set[uniqueFieldName] = updatedValue;

            var res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {length: 1},  // fetch document of smallest size
                update: update,
                new: true
            });
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertWhenOwnColl(doc !== null, 'query spec should have matched a document');

            if (doc === null) {
                return;
            }

            assertAlways.eq(this.tid, doc.tid);
            assertWhenOwnColl.eq(updatedValue, doc[uniqueFieldName]);
            assertWhenOwnColl.eq(updatedLength, doc.length);

            this.length = updatedLength;
            this.bsonsize = Object.bsonsize(doc);

            // Get the DiskLoc of the document after its potential move
            var after = db[collName].find({_id: before._id}).showDiskLoc().next();

            if (isMongod(db) && isMMAPv1(db)) {
                // Since the document has at least doubled in size, and the default
                // allocation strategy of mmapv1 is to use power of two sizes, the
                // document will have always moved
                assertWhenOwnColl.neq(
                    before.$recordId, after.$recordId, 'document should have moved');
            }
        }

        return {
            insert: insert,
            findAndModify: findAndModify,
        };

    })();

    var transitions = {insert: {findAndModify: 1}, findAndModify: {findAndModify: 1}};

    return {
        threadCount: 20,
        iterations: 20,
        data: data,
        states: states,
        startState: 'insert',
        transitions: transitions
    };

})();
