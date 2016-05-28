'use strict';

/**
 * update_multifield_isolated_multiupdate.js
 *
 * Does updates that affect multiple fields on multiple documents, using $isolated.
 * The collection has an index for each field, and a multikey index for all fields.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_multifield.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.multi = true;
    $config.data.isolated = true;

    $config.data.assertResult = function assertResult(res, db, collName, query) {
        assertAlways.eq(0, res.nUpserted, tojson(res));
        // documents can't move during an update, because we use $isolated
        assertWhenOwnColl.eq(this.numDocs, res.nMatched, tojson(res));
        if (db.getMongo().writeMode() === 'commands') {
            assertWhenOwnColl.eq(this.numDocs, res.nModified, tojson(res));
        }

        // every thread only increments z, and z starts at 0,
        // so z should always be strictly greater than 0 after an update,
        // even if other threads modify the doc.
        var docs = db[collName].find().toArray();
        assertWhenOwnColl(function() {
            docs.forEach(function(doc) {
                assertWhenOwnColl.eq('number', typeof doc.z);
                assertWhenOwnColl.gt(doc.z, 0);
            });
        });
    };

    return $config;
});
