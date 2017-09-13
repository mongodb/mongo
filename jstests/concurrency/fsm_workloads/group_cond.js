'use strict';

/**
 * group_cond.js
 *
 * Inserts 1000 documents with a field set to a random
 * float value.  The group command is then used to partition these documents
 * into one of ten buckets:
 * [0, 0.09x), [0.10, 0.19x), ..., [0.80, 0.89x), [0.90, 1.0)
 *
 * To increase testing coverage, the float field is indexed and
 * a 'cond' document is supplied to the group command.
 *
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/group.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        assertAlways.commandWorked(db[collName].ensureIndex({rand: 1}));
    };

    $config.states.group = function group(db, collName) {
        var cmdObj = this.generateGroupCmdObj(collName);
        cmdObj.group.cond = {rand: {$gte: 0.5}};
        var res = db.runCommand(cmdObj);
        assertWhenOwnColl.commandWorked(res);

        assertWhenOwnColl.lte(res.count, this.numDocs);
        assertWhenOwnColl.lte(res.keys, 5);
        assertWhenOwnColl(function() {
            assertWhenOwnColl.lte(res.retval.length, 5);
            assertWhenOwnColl.eq(this.sumBucketCount(res.retval), res.count);
        }.bind(this));
    };

    return $config;
});
