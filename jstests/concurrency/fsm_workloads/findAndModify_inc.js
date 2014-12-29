/**
 * findAndModify_inc.js
 *
 * Inserts a single document into a collection. Each thread performs a
 * findAndModify command to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 *
 * This workload was designed to reproduce SERVER-15892.
 */
var $config = (function() {

    var states = {

        init: function init(db, collName) {
            this.fieldName = 't' + this.tid;
            this.count = 0;
        },

        update: function update(db, collName) {
            var updateDoc = { $inc: {} };
            updateDoc.$inc[this.fieldName] = 1;
            db[collName].findAndModify({
                query: { _id: 'findAndModify_inc' },
                update: updateDoc
            });
            ++this.count;
        },

        find: function find(db, collName) {
            var docs = db[collName].find().toArray();
            assertWhenOwnColl.eq(1, docs.length);
            assertWhenOwnColl((function() {
                var doc = docs[0];
                assertWhenOwnColl.eq(this.count, doc[this.fieldName]);
            }).bind(this));
        }

    };

    var transitions = {
        init: { update: 1 },
        update: { find: 1 },
        find: { update: 1 }
    };

    function setup(db, collName) {
        db[collName].insert({ _id: 'findAndModify_inc' });
    }

    return {
        threadCount: 30,
        iterations: 100,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
