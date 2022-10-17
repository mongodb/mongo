/**
 * Tests TTL indexes with invalid values for 'expireAfterSeconds'.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

function test(expireAfterSecondsVal) {
    jsTestLog("Testing expireAfterSeconds = " + expireAfterSecondsVal);

    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
        nodeOptions: {setParameter: {ttlMonitorSleepSecs: 5}},
        // Sync from primary only so that we have a well-defined node to check listIndexes behavior.
        settings: {chainingAllowed: false},
    });
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    const db = primary.getDB('test');
    const coll = db.t;

    // The test cases here revolve around having a TTL index in the catalog with an invalid
    // 'expireAfterSeconds'. The current createIndexes behavior will reject index creation for
    // invalid values of expireAfterSeconds, so we use a failpoint to disable that checking to
    // simulate a value leftover from very old MongoDB versions.
    const fp = configureFailPoint(primary, 'skipTTLIndexValidationOnCreateIndex');
    const fp2 = configureFailPoint(primary,
                                   'skipTTLIndexInvalidExpireAfterSecondsValidationForCreateIndex');
    try {
        assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: expireAfterSecondsVal}));
    } finally {
        fp.off();
        fp2.off();
    }

    assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

    // Log the contents of the catalog for debugging purposes in case of failure.
    let catalogContents = coll.aggregate([{$listCatalog: {}}]).toArray();
    jsTestLog("Catalog contents: " + tojson(catalogContents));

    // Wait for "Skipping TTL job due to invalid index spec" log message.
    checkLog.containsJson(primary, 6909100, {ns: coll.getFullName()});

    // TTL index should be replicated to the secondary with an invalid 'expireAfterSeconds'.
    const secondary = rst.getSecondary();
    checkLog.containsJson(secondary, 20384, {
        namespace: coll.getFullName(),
        properties: (spec) => {
            jsTestLog('TTL index on secondary: ' + tojson(spec));
            // NaN does not equal NaN, so we have to use the isNaN function for this check.
            if (isNaN(expireAfterSecondsVal))
                return isNaN(spec.expireAfterSeconds);
            else
                return spec.expireAfterSeconds == expireAfterSecondsVal;
        }
    });

    assert.eq(coll.countDocuments({}),
              1,
              'ttl index with invalid expireAfterSeconds should not remove any documents.');

    // Confirm that TTL index is replicated with a non-zero 'expireAfterSeconds' during initial
    // sync.
    const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
    rst.reInitiate();
    rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
    rst.awaitReplication();
    let newNodeTestDB = newNode.getDB(db.getName());
    let newNodeColl = newNodeTestDB.getCollection(coll.getName());
    const newNodeIndexes = IndexBuildTest.assertIndexes(newNodeColl, 2, ['_id_', 't_1']);
    const newNodeSpec = newNodeIndexes.t_1;
    jsTestLog('TTL index on initial sync node: ' + tojson(newNodeSpec));
    assert(newNodeSpec.hasOwnProperty('expireAfterSeconds'),
           'Index was not replicated as a TTL index during initial sync.');
    assert.eq(
        newNodeSpec.expireAfterSeconds,
        2147483647,  // This is the "disabled" value for expireAfterSeconds
        expireAfterSecondsVal +
            ' expireAferSeconds was replicated as something other than disabled during initial sync.');

    // Check that listIndexes on the primary logged a "Fixing expire field from TTL index spec"
    // message during the invalid 'expireAfterSeconds' conversion.
    checkLog.containsJson(primary, 6835900, {namespace: coll.getFullName()});

    // Confirm that a node with an existing TTL index with an invalid 'expireAfterSeconds' will
    // convert the duration on the TTL index from the invalid value to a large positive value when
    // it becomes the primary node. When stepping down the primary, we use 'force' because there's
    // no other electable node.  Subsequently, we wait for the stepped down node to become primary
    // again.  To confirm that the TTL index has been fixed, we check the oplog for a collMod
    // operation on the TTL index that changes the `expireAfterSeconds` field from the invalid value
    // to a large positive value.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 5, force: true}));
    primary = rst.waitForPrimary();

    // Log the contents of the catalog for debugging purposes in case of failure.
    let newPrimaryColl = primary.getDB(db.getName()).getCollection(coll.getName());
    const newPrimaryCatalogContents = newPrimaryColl.aggregate([{$listCatalog: {}}]).toArray();
    jsTestLog("Catalog contents on new primary: " + tojson(newPrimaryCatalogContents));

    const collModOplogEntries =
        rst.findOplog(primary,
                      {
                          op: 'c',
                          ns: newPrimaryColl.getDB().getCollection('$cmd').getFullName(),
                          'o.collMod': coll.getName(),
                          'o.index.name': 't_1',
                          'o.index.expireAfterSeconds': newNodeSpec.expireAfterSeconds
                      },
                      /*limit=*/1)
            .toArray();
    assert.eq(collModOplogEntries.length,
              1,
              'TTL index with ' + expireAfterSecondsVal +
                  ' expireAfterSeconds was not fixed using collMod during step-up: ' +
                  tojson(rst.findOplog(primary, {op: {$ne: 'n'}}, /*limit=*/10).toArray()));

    rst.stopSet();
}

test(NaN);
const maxDouble = 1.7976931348623157e+308;
test(maxDouble);
test(-maxDouble);
})();
