/**
 * rename_collection_droptarget.js
 *
 * Creates a collection and then repeatedly executes the renameCollection
 * command against it. Inserts documents into the "to" namespace and specifies
 * dropTarget=true.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
export const $config = (function() {
    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'rename_collection_droptarget'
    };

    var states = (function() {
        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function insert(db, collName, numDocs) {
            for (var i = 0; i < numDocs; ++i) {
                var res = db[collName].insert({});
                assert.commandWorked(res);
                assert.eq(1, res.nInserted);
            }
        }

        function init(db, collName) {
            var num = 0;
            this.fromCollName = uniqueCollectionName(this.prefix, this.tid, num++);
            this.toCollName = uniqueCollectionName(this.prefix, this.tid, num++);

            assert.commandWorked(db.createCollection(this.fromCollName));
        }

        function rename(db, collName) {
            // Clear out the "from" collection and insert 'fromCollCount' documents
            var fromCollCount = 7;
            assert(db[this.fromCollName].drop());
            assert.commandWorked(db.createCollection(this.fromCollName));
            insert(db, this.fromCollName, fromCollCount);

            var toCollCount = 4;
            assert.commandWorked(db.createCollection(this.toCollName));
            insert(db, this.toCollName, toCollCount);

            // Verify that 'fromCollCount' documents exist in the "to" collection
            // after the rename occurs
            var res =
                db[this.fromCollName].renameCollection(this.toCollName, true /* dropTarget */);

            // SERVER-57128: NamespaceNotFound is an acceptable error if the mongos retries
            // the rename after the coordinator has already fulfilled the original request
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.NamespaceNotFound);

            assert.eq(fromCollCount, db[this.toCollName].find().itcount());
            assert.eq(0, db[this.fromCollName].find().itcount());

            // Swap "to" and "from" collections for next execution
            var temp = this.fromCollName;
            this.fromCollName = this.toCollName;
            this.toCollName = temp;
        }

        return {init: init, rename: rename};
    })();

    var transitions = {init: {rename: 1}, rename: {rename: 1}};

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
    };
})();
