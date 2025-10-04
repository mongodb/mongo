/**
 * findAndModify_update.js
 *
 * Each thread inserts multiple documents into a collection, and then
 * repeatedly performs the findAndModify command. A single document is
 * selected based on 'query' and 'sort' specifications, and updated
 * using either the $min or $max operator.
 */
export const $config = (function () {
    let data = {
        numDocsPerThread: 3, // >1 for 'sort' to be meaningful
        shardKey: {tid: 1},
    };

    let states = (function () {
        function makeDoc(tid) {
            return {_id: new ObjectId(), tid: tid, value: 0};
        }

        function init(db, collName) {
            for (let i = 0; i < this.numDocsPerThread; ++i) {
                let res = db[collName].insert(makeDoc(this.tid));
                assert.commandWorked(res);
                assert.eq(1, res.nInserted);
            }
        }

        function findAndModifyAscending(db, collName) {
            let updatedValue = this.tid;

            let res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {value: 1},
                update: {$max: {value: updatedValue}},
                new: true,
            });
            assert.commandWorked(res);

            let doc = res.value;
            assert(doc !== null, "query spec should have matched a document, returned " + tojson(res));

            if (doc !== null) {
                assert.eq(this.tid, doc.tid);
                assert.eq(updatedValue, doc.value);
            }
        }

        function findAndModifyDescending(db, collName) {
            let updatedValue = -this.tid;

            let res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {value: -1},
                update: {$min: {value: updatedValue}},
                new: true,
            });
            assert.commandWorked(res);

            let doc = res.value;
            assert(doc !== null, "query spec should have matched a document, returned " + tojson(res));

            if (doc !== null) {
                assert.eq(this.tid, doc.tid);
                assert.eq(updatedValue, doc.value);
            }
        }

        return {
            init: init,
            findAndModifyAscending: findAndModifyAscending,
            findAndModifyDescending: findAndModifyDescending,
        };
    })();

    let transitions = {
        init: {findAndModifyAscending: 0.5, findAndModifyDescending: 0.5},
        findAndModifyAscending: {findAndModifyAscending: 0.5, findAndModifyDescending: 0.5},
        findAndModifyDescending: {findAndModifyAscending: 0.5, findAndModifyDescending: 0.5},
    };

    function setup(db, collName, cluster) {
        let res = db[collName].createIndex({tid: 1, value: 1});
        assert.commandWorked(res);
    }

    return {
        threadCount: 20,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
