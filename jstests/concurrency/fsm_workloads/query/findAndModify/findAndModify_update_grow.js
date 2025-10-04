/**
 * findAndModify_update_grow.js
 *
 * Each thread inserts a single document into a collection, and then
 * repeatedly performs the findAndModify command. Checks that document
 * moves don't happen and that large changes in document size are handled
 * correctly.
 *
 * The findAndModify_update_grow.js workload can cause OOM kills on test hosts;
 * therefore it is run only on standalones.
 * @tags: [requires_standalone]
 *
 */
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let data = {
        shardKey: {tid: 1},
    };

    let states = (function () {
        // Use the workload name as the field name (since it is assumed
        // to be unique) to avoid any potential issues with large keys
        // and indexes on the collection.
        let uniqueFieldName = "findAndModify_update_grow";

        function makeStringOfLength(length) {
            return "x".repeat(length);
        }

        function makeDoc(tid) {
            // Use 32-bit integer for representing 'length' property
            // to ensure $mul does integer multiplication
            let doc = {_id: new ObjectId(), tid: tid, length: new NumberInt(1)};
            doc[uniqueFieldName] = makeStringOfLength(doc.length);
            return doc;
        }

        function insert(db, collName) {
            let doc = makeDoc(this.tid);
            this.length = doc.length;
            this.bsonsize = Object.bsonsize(doc);

            let res = db[collName].insert(doc);
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }

        function findAndModify(db, collName) {
            // When the size of the document starts to near the 16MB limit,
            // start over with a new document
            if (this.bsonsize > 4 * 1024 * 1024 /* 4MB */) {
                insert.call(this, db, collName);
            }

            // Get the DiskLoc of the document before its potential move
            let before = db[collName]
                .find({tid: this.tid})
                .showDiskLoc()
                .sort({length: 1}) // fetch document of smallest size
                .limit(1)
                .next();

            // Increase the length of the 'findAndModify_update_grow' string
            // to double the size of the overall document
            let factor = Math.ceil((2 * this.bsonsize) / this.length);
            let updatedLength = factor * this.length;
            let updatedValue = makeStringOfLength(updatedLength);

            let update = {$set: {}, $mul: {length: factor}};
            update.$set[uniqueFieldName] = updatedValue;

            let res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {length: 1}, // fetch document of smallest size
                update: update,
                new: true,
            });
            assert.commandWorked(res);

            let doc = res.value;
            assert(doc !== null, "query spec should have matched a document, returned " + tojson(res));

            if (doc === null) {
                return;
            }

            assert.eq(this.tid, doc.tid);
            assert.eq(updatedValue, doc[uniqueFieldName]);
            assert.eq(updatedLength, doc.length);

            this.length = updatedLength;
            this.bsonsize = Object.bsonsize(doc);

            // Get the DiskLoc of the document after its potential move
            let after = db[collName].find({_id: before._id}).showDiskLoc().next();

            if (isMongod(db)) {
                // Even though the document has at least doubled in size, the document
                // must never move.
                assert.eq(before.$recordId, after.$recordId, "document should not have moved");
            }
        }

        return {
            insert: insert,
            findAndModify: findAndModify,
        };
    })();

    let transitions = {insert: {findAndModify: 1}, findAndModify: {findAndModify: 1}};

    return {
        threadCount: 20,
        iterations: 20,
        data: data,
        states: states,
        startState: "insert",
        transitions: transitions,
    };
})();
