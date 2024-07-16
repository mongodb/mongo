/**
 * findAndModify_upsert.js
 *
 * Each thread repeatedly performs the findAndModify command, specifying
 * upsert as either true or false. A single document is selected (or
 * created) based on the 'query' specification, and updated using the
 * $push operator.
 */
export const $config = (function() {
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
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.StaleConfig);

            if (res.ok === 1) {
                var doc = res.value;
                assert(doc !== null, 'a document should have been inserted');

                assert.eq(this.tid, doc.tid);
                assert(Array.isArray(doc.values), 'expected values to be an array');
                assert.eq(1, doc.values.length);
                assert.eq(updatedValue, doc.values[0]);
            }
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
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.StaleConfig);

            if (res.ok === 1) {
                var doc = res.value;
                assert(doc !== null,
                       'query spec should have matched a document, returned ' + tojson(res));

                assert.eq(this.tid, doc.tid);
                assert(Array.isArray(doc.values), 'expected values to be an array');
                assert.gte(doc.values.length, 2);
                assert.eq(updatedValue, doc.values[doc.values.length - 1]);
                assert(isSorted(doc.values), 'expected values to be sorted: ' + tojson(doc.values));
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
