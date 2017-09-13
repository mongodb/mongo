'use strict';

/**
 * group.js
 *
 * Inserts 1000 documents with a field set to a random
 * float value.  The group command is then used to partition these documents
 * into one of ten buckets:
 * [0, 0.09x), [0.10, 0.19x), ..., [0.80, 0.89x), [0.90, 1.0)
 *
 * The float field is not indexed.
 *
 */

var $config = (function() {

    function generateGroupCmdObj(collName) {
        return {
            group: {
                ns: collName,
                initial: {bucketCount: 0, bucketSum: 0},
                $keyf: function $keyf(doc) {
                    // place doc.rand into appropriate bucket
                    return {bucket: Math.floor(doc.rand * 10) + 1};
                },
                $reduce: function $reduce(curr, result) {
                    result.bucketCount++;
                    result.bucketSum += curr.rand;
                },
                finalize: function finalize(result) {
                    // calculate average float value per bucket
                    result.bucketAvg = result.bucketSum / (result.bucketCount || 1);
                }
            }
        };
    }

    function sumBucketCount(arr) {
        return arr.reduce(function(a, b) {
            return a + b.bucketCount;
        }, 0);
    }

    var data = {
        numDocs: 1000,
        generateGroupCmdObj: generateGroupCmdObj,
        sumBucketCount: sumBucketCount
    };

    var states = (function() {

        function group(db, collName) {
            var res = db.runCommand(this.generateGroupCmdObj(collName));
            assertWhenOwnColl.commandWorked(res);

            assertWhenOwnColl.lte(res.count, this.numDocs);
            assertWhenOwnColl.lte(res.keys, 10);
            assertWhenOwnColl(function() {
                assertWhenOwnColl.lte(res.retval.length, 10);
                assertWhenOwnColl.eq(this.sumBucketCount(res.retval), res.count);
            }.bind(this));
        }

        return {group: group};

    })();

    var transitions = {group: {group: 1}};

    function setup(db, collName, cluster) {
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            bulk.insert({rand: Random.rand()});
        }
        var res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(this.numDocs, res.nInserted);
    }

    function teardown(db, collName, cluster) {
        assertWhenOwnColl(db[collName].drop());
    }

    return {
        // Using few threads and iterations because each iteration per thread
        // is fairly expensive compared to other workloads' iterations.
        threadCount: 3,
        iterations: 10,
        startState: 'group',
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown
    };

})();
