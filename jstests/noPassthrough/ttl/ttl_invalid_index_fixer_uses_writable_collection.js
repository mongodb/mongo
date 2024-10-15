/**
 * Reproduces a seg-fault caused by the InvalidTTLIndexFixer trying to 'fix' an invalid TTL index
 * spec with both a non-int 'expireAfterSeconds' field and an unexpected additional field.
 * Specifically, the behavior is caused by the additional field being an accepted field for text
 * indexes, 'weights', but not indexes of other types.
 *
 * The original issue occured when trying to upgrade from an older version, so the first steps are
 * to bypass validation checks and introduce an invalid ttl index spec.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}},
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = rst.getPrimary();
let secondary = rst.getSecondary();
const db = primary.getDB('test');
const expireAfterSecondsNonInt = 100.3;
let coll = db.t;
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// Failpoints to circumvent validation when introducing the invalid index. It's possible we don't
// need all of them. 'skipIndexCreateWeightsFieldValidation' is the most important.
const fpNames = [
    'skipTTLIndexValidationOnCreateIndex',
    'skipTTLIndexExpireAfterSecondsValidation',
    'skipIndexCreateFieldNameValidation',
    'skipIndexCreateWeightsFieldValidation',
];
const fps = [];
for (const fpName of fpNames) {
    fps.push(configureFailPoint(primary, fpName));
    fps.push(configureFailPoint(secondary, fpName));
}
try {
    assert.commandWorked(
        coll.createIndex({t: 1}, {expireAfterSeconds: expireAfterSecondsNonInt, weights: {}}));
    const catalogContents = coll.aggregate([{$listCatalog: {}}]).toArray();
    jsTestLog("Catalog contents: " + tojson(catalogContents));

    rst.awaitReplication();
    rst.awaitLastStableRecoveryTimestamp();
} finally {
    for (const fp of fps) {
        fp.off();
    }
}

// When the primary steps back up, we eventually trigger the seg-fault.
jsTest.log(
    `Forcing the primary to step down then step up again to trigger 'InvalidTTLIndexFixer' thread`);
assert.commandWorked(primary.adminCommand({replSetStepDown: 5, force: true}));
primary = rst.waitForPrimary();

coll = primary.getDB(db.getName()).getCollection(coll.getName());
jsTestLog("Catalog contents on primary: " + tojson(coll.aggregate([{$listCatalog: {}}]).toArray()));

// Ensure the 'InvalidTTLIndexFixer' thread has time to run.
assert.soon(
    () => {
        return 1 ==
            rst.findOplog(primary,
                          {
                              op: 'c',
                              ns: coll.getDB().getCollection('$cmd').getFullName(),
                              'o.collMod': coll.getName(),
                              'o.index.name': 't_1',
                              'o.index.expireAfterSeconds': Math.floor(expireAfterSecondsNonInt),
                          },
                          /*limit=*/ 1)
                .toArray()
                .length;
    },
    'TTL index with ' + expireAfterSecondsNonInt +
        ' expireAfterSeconds was not fixed using collMod during step-up: ' +
        tojson(rst.findOplog(primary, {op: {$ne: 'n'}}, /*limit=*/ 10).toArray()));
rst.awaitReplication();
rst.stopSet();
