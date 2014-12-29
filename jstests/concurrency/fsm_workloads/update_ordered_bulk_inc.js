/**
 * update_ordered_bulk_inc.js
 *
 * Inserts multiple documents into a collection. Each thread performs a
 * bulk update operation to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 *
 * Uses an ordered, bulk operation to perform the updates.
 */
var $config = (function() {

    var states = {
        init: function init(db, collName) {
            this.fieldName = 't' + this.tid;
        },

        update: function update(db, collName) {
            var updateDoc = { $inc: {} };
            updateDoc.$inc[this.fieldName] = 1;

            var bulk = db[collName].initializeOrderedBulkOp();
            for (var i = 0; i < this.docCount; ++i) {
                bulk.find({ _id: i }).update(updateDoc);
            }
            var result = bulk.execute();
            // TODO: this actually does assume that there are no unique indexes.
            //       but except for weird cases like that, it is valid even when other
            //       threads are modifying the same collection
            assertAlways.eq(0, result.getWriteErrorCount());

            ++this.count;
        },

        find: function find(db, collName) {
            var docs = db[collName].find().toArray();
            assertWhenOwnColl.eq(this.docCount, docs.length);

            docs.forEach(function (doc) {
                assertWhenOwnColl.eq(this.count, doc[this.fieldName]);
            });
        }
    };

    var transitions = {
        init: { update: 1 },
        update: { find: 1 },
        find: { update: 1 }
    };

    function setup(db, collName) {
        this.count = 0;
        for (var i = 0; i < this.docCount; ++i) {
            db[collName].insert({ _id: i });
        }
    }

    return {
        threadCount: 30,
        iterations: 100,
        states: states,
        transitions: transitions,
        setup: setup,
        data: {
            docCount: 15
        }
    };

})();
