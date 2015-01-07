'use strict';

/**
 * update_multifield_multiupdate.js
 *
 * Does updates that affect multiple fields on multiple documents.
 * The collection has an index for each field, and a multikey index for all fields.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js'); // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_multifield.js'); // for $config
load('jstests/concurrency/fsm_workload_helpers/server_types.js'); // for isMongod and isMMAPv1

var $config = extendWorkload($config, function($config, $super) {

    $config.data.multi = true;

    $config.data.assertResult = function(res, db, collName, query) {
        assertAlways.eq(0, res.nUpserted, tojson(res));

        var status = db.serverStatus();
        if (isMongod(status)) {
            if (isMMAPv1(status)) {
                // If an update triggers a document to move forward, then
                // that document can be matched multiple times. If an update
                // triggers a document to move backwards, then that document
                // can be missed by other threads.
                assertAlways.gte(res.nMatched, 0, tojson(res));
            } else { // non-mmapv1 storage engine
                // TODO: Can we assert exact equality with WiredTiger?
                //       What about for other storage engines?
                assertWhenOwnColl.lte(this.numDocs, res.nMatched, tojson(res));
            }
        } else { // mongos
            // In a mixed cluster, it is unknown what underlying storage engine
            // the update operations will be executed against. Thus, we can only
            // make the weakest of all assertions above.
            assertAlways.gte(res.nMatched, 0, tojson(res));
        }

        if (db.getMongo().writeMode() === 'commands') {
            assertWhenOwnColl.eq(res.nMatched, res.nModified, tojson(res));
        }

        var docs = db[collName].find().toArray();
        docs.forEach(function(doc) {
            assertWhenOwnColl.eq('number', typeof doc.z);
            assertWhenOwnColl.gt(doc.z, 0);
        });
    };

    return $config;
});
