/**
 * Performs concurrent upsert and delete operations against a small set of documents with a unique
 * index in place. One specific scenario this test exercises is upsert retry in the case where an
 * upsert generates an insert, which then fails due to another operation inserting first. This test
 * also involves collation.
 */
export const $config = (function() {
    const data = {
        uniqueKeys: ["aa", "ab", "ba", "bb", "Aa", "Ab", "Ba", "BB"],
        getUniqueKey: function() {
            return this.uniqueKeys[Random.randInt(this.uniqueKeys.length)];
        },
        originalReshardingOplogBatchTaskCount: {},
    };

    const states = {
        delete: function(db, collName) {
            assert.commandWorked(
                db[collName].remove({uniqueKey: this.getUniqueKey()}, {justOne: true}));
        },
        upsert: function(db, collName) {
            const uniqueKey = this.getUniqueKey();
            const cmdRes = db.runCommand({
                update: collName,
                updates: [{
                    q: {uniqueKey: uniqueKey},
                    u: {$inc: {updates: 1}},
                    upsert: true,
                    collation: {locale: "en", strength: 2},
                }]
            });
            assert.commandWorked(cmdRes);
        },
    };

    const transitions = {
        upsert: {upsert: 0.5, delete: 0.5},
        delete: {upsert: 0.5, delete: 0.5},
    };

    function setup(db, collName, cluster) {
        db[collName].drop();
        assert.commandWorked(db[collName].createIndex(
            {uniqueKey: 1}, {unique: 1, collation: {locale: "en", strength: 2}}));

        // Creating an unique index on a key other than _id can result in DuplicateKey errors for
        // background resharding operations. To avoid this, lower reshardingOplogBatchTaskCount
        // to 1.
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                reshardingOplogBatchTaskCount: 1,
            }));
            this.originalReshardingOplogBatchTaskCount[db.getMongo().host] = res.was;
        });
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                reshardingOplogBatchTaskCount:
                    this.originalReshardingOplogBatchTaskCount[db.getMongo().host],
            }));
        });
    }

    return {
        threadCount: 20,
        iterations: 100,
        states: states,
        startState: 'upsert',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
