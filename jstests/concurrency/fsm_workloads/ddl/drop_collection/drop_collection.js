/**
 * drop_collection.js
 *
 * Repeatedly creates and drops a collection.
 */
export const $config = (function () {
    let data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: "drop_collection",
    };

    let states = (function () {
        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + "_" + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createAndDrop(db, collName) {
            // TODO: should we ever do something different?
            let myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
            assert.commandWorked(db.createCollection(myCollName));
            assert(db[myCollName].drop());
        }

        return {init: init, createAndDrop: createAndDrop};
    })();

    let transitions = {init: {createAndDrop: 1}, createAndDrop: {createAndDrop: 1}};

    return {threadCount: 10, iterations: 10, data: data, states: states, transitions: transitions};
})();
