'use strict';

/**
 * findAndModify_upsert.js
 *
 * Each thread repeatedly performs the findAndModify command, specifying
 * upsert as either true or false. A single document is selected (or
 * created) based on the 'query' specification, and updated using the
 * $push operator.
 */
var $config = (function() {

    var data = {sort: false, shardKey: {tid: 1}};

    var states = (function() {

        // Returns true if the specified array is sorted in ascending order,
        // and false otherwise.
        function isSorted(arr) {
            for (var i = 0; i < arr.length - 1; ++i) {
                if (arr[i] > arr[i + 1]) {
                    return false;
                }
            }

            return true;
        }

        function init(db, collName) {
            this.iter = 0;

            // Need to guarantee that an upsert has occurred prior to an update,
            // which is not enforced by the transition table under composition
            upsert.call(this, db, collName);
        }

        function upsert(db, collName) {
            var updatedValue = this.iter++;

            // Use a query specification that does not match any existing documents
            var query = {_id: new ObjectId(), tid: this.tid};

            var cmdObj = {
                findandmodify: db[collName].getName(),
                query: query,
                update: {$setOnInsert: {values: [updatedValue]}},
                new: true,
                upsert: true
            };

            if (this.sort) {
                cmdObj.sort = this.sort;
            }

            var res = db.runCommand(cmdObj);
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertAlways(doc !== null, 'a document should have been inserted');

            assertAlways((function() {
                             assertAlways.eq(this.tid, doc.tid);
                             assertAlways(Array.isArray(doc.values),
                                          'expected values to be an array');
                             assertAlways.eq(1, doc.values.length);
                             assertAlways.eq(updatedValue, doc.values[0]);
                         }).bind(this));
        }

        function update(db, collName) {
            var updatedValue = this.iter++;

            var cmdObj = {
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                update: {$push: {values: updatedValue}},
                new: true,
                upsert: false
            };

            if (this.sort) {
                cmdObj.sort = this.sort;
            }

            var res = db.runCommand(cmdObj);
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertWhenOwnColl(doc !== null, 'query spec should have matched a document');

            if (doc !== null) {
                assertAlways.eq(this.tid, doc.tid);
                assertWhenOwnColl(Array.isArray(doc.values), 'expected values to be an array');
                assertWhenOwnColl(function() {
                    assertWhenOwnColl.gte(doc.values.length, 2);
                    assertWhenOwnColl.eq(updatedValue, doc.values[doc.values.length - 1]);
                    assertWhenOwnColl(isSorted(doc.values),
                                      'expected values to be sorted: ' + tojson(doc.values));
                });
            }
        }

        return {init: init, upsert: upsert, update: update};

    })();

    var transitions = {
        init: {upsert: 0.1, update: 0.9},
        upsert: {upsert: 0.1, update: 0.9},
        update: {upsert: 0.1, update: 0.9}
    };

    return {threadCount: 20, iterations: 20, data: data, states: states, transitions: transitions};

})();
