/**
 * indexed_insert_where.js
 *
 * Bulk inserts documents in batches of 100, and then queries for documents inserted by the thread.
 * Note: This workload is extended by remove_where.js, update_where.js, and upsert_where.js.
 * data.insertedDocuments is used as a counter by all of those workloads for their own checks.
 * @tags: [
 *  # Uses $where operator
 *  requires_scripting,
 *  requires_getmore,
 *  # TODO (SERVER-91002): server side javascript execution is deprecated, and the balancer is not
 *  # compatible with it, once the incompatibility is taken care off we can re-enable this test
 *  assumes_balancer_off
 * ]
 */
export const $config = (function() {
    var data = {
        documentsToInsert: 100,
        insertedDocuments: 0,
        generateDocumentToInsert: function generateDocumentToInsert() {
            return {tid: this.tid};
        },
        shardKey: {tid: 1}
    };

    var states = {
        insert: function insert(db, collName) {
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.documentsToInsert; ++i) {
                bulk.insert(this.generateDocumentToInsert());
            }
            var res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.documentsToInsert, res.nInserted);
            this.insertedDocuments += this.documentsToInsert;
        },

        query: function query(db, collName) {
            var count = db[collName].find({$where: 'this.tid === ' + this.tid}).itcount();
            assert.eq(count,
                      this.insertedDocuments,
                      '$where query should return the number of documents this ' +
                          'thread inserted');
        }
    };

    var transitions = {insert: {insert: 0.2, query: 0.8}, query: {insert: 0.8, query: 0.2}};

    var setup = function setup(db, collName, cluster) {
        assert.commandWorked(db[collName].createIndex({tid: 1}));
    };

    return {
        threadCount: 10,
        iterations: 10,
        data: data,
        states: states,
        startState: 'insert',
        setup: setup,
        transitions: transitions
    };
})();
