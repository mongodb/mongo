/**
 * update_rename.js
 *
 * Each thread does a $rename to cause documents to jump between indexes.
 */
export const $config = (function () {
    let fieldNames = ["update_rename_x", "update_rename_y", "update_rename_z"];

    function choose(array) {
        assert.gt(array.length, 0, "can't choose an element of an empty array");
        return array[Random.randInt(array.length)];
    }

    let states = {
        update: function update(db, collName) {
            let from = choose(fieldNames);
            let to = choose(
                fieldNames.filter(function (n) {
                    return n !== from;
                }),
            );
            let updater = {$rename: {}};
            updater.$rename[from] = to;

            let query = {};
            query[from] = {$exists: 1};

            let res = db[collName].update(query, updater);

            assert.eq(0, res.nUpserted, tojson(res));
            assert.contains(res.nMatched, [0, 1], tojson(res));
            assert.eq(res.nMatched, res.nModified, tojson(res));
        },
    };

    let transitions = {update: {update: 1}};

    function setup(db, collName, cluster) {
        // Create an index on all but one fieldName key to make it possible to test renames
        // between indexed fields and non-indexed fields
        fieldNames.slice(1).forEach(function (fieldName) {
            let indexSpec = {};
            indexSpec[fieldName] = 1;
            assert.commandWorked(db[collName].createIndex(indexSpec));
        });

        // numDocs should be much less than threadCount, to make more threads use the same docs.
        this.numDocs = Math.floor(this.threadCount / 5);
        assert.gt(this.numDocs, 0, "numDocs should be a positive number");

        for (let i = 0; i < this.numDocs; ++i) {
            let fieldName = fieldNames[i % fieldNames.length];
            let doc = {};
            doc[fieldName] = i;
            let res = db[collName].insert(doc);
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 20,
        iterations: 20,
        startState: "update",
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
