'use strict';

/**
 * Performs a series of {multi: true} updates and deletes. At the end of the workload it checks that
 * no change stream events are generated due to writes on orphan documents.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  featureFlagNoChangeStreamEventsDueToOrphans
 * ];
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    $config.states.multiUpdate = function(db, collName, connCache) {
        const collection = db[collName];
        const id = this.getIdForThread(collName);
        assertWhenOwnColl.commandWorked(
            collection.update({x: id}, {$inc: {counter: 1}}, {multi: true}));
    };

    $config.states.multiDelete = function(db, collName, connCache) {
        const collection = db[collName];
        const id = this.getIdForThread(collName);
        assertWhenOwnColl.commandWorked(collection.remove({x: id}, {multi: true}));
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        db[collName].find({}).forEach(doc => {
            db[collName].update({_id: doc._id}, {$set: {x: doc._id, counter: 0}});
        });

        // Open a change stream on the collection
        const collection = db.getCollection(collName);
        $config.data.changeStream = collection.watch();
    };

    $config.teardown = function teardown(db, collName, cluster) {
        // Check that no duplicate events are seen on the change stream.
        // - For deletes this means that we should not see the same document deleted more than once
        // (since the workload does not perform any inserts after setup).
        // - For updates, this means that for each document, we should never see the same updated
        // value more than once. This is because the updates are {$inc: 1}, so they must be strictly
        // incrementing.
        var seenDeletes = [];
        var seenUpdates = [];
        assert.soon(() => $config.data.changeStream.hasNext());
        while ($config.data.changeStream.hasNext()) {
            let event = $config.data.changeStream.next();

            if (event.operationType == "delete") {
                assert(!seenDeletes.includes(event.documentKey._id),
                       "Found duplicate change stream event for delete on _id: " +
                           event.documentKey._id);
                seenDeletes.push(event.documentKey._id);
            } else if (event.operationType == "update") {
                const idAndUpdate = {
                    _id: event.documentKey._id,
                    updatedFields: event.updateDescription.updatedFields
                };

                assert(!seenUpdates.some(
                           item => item._id === idAndUpdate._id &&
                               bsonWoCompare(item.updatedFields, idAndUpdate.updatedFields) == 0),
                       "Found duplicate change stream event for update on _id: " +
                           event.documentKey._id +
                           ", update: " + tojson(event.updateDescription.updatedFields));
                seenUpdates.push(idAndUpdate);
            }
        }

        $super.teardown.apply(this, arguments);
    };

    $config.transitions = {
        init: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        moveChunk: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        multiUpdate: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        multiDelete: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
    };

    return $config;
});
