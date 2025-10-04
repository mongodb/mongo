// Test that oplog application on the secondary happens correctly when the collection has a default
// collation and operations by _id which must respect the collation are issued.
import {ReplSetTest} from "jstests/libs/replsettest.js";

Random.setRandomSeed();

// Return a string whose character at index 'i' in 'str' is replaced by 'character'.
function replaceChar(str, i, character) {
    assert.eq(1, character.length);
    return str.substr(0, i) + character + str.substr(i + 1);
}

// Return a string whose character at index 'i' has been uppercased.
function uppercaseIth(str, i) {
    return replaceChar(str, i, str[i].toUpperCase());
}

const caseInsensitive = {
    collation: {locale: "en_US", strength: 2},
};

let replTest = new ReplSetTest({name: "testSet", nodes: 2});
let nodes = replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
let primaryDB = primary.getDB("test");
let primaryColl = primaryDB.collate_id;

let secondary = replTest.getSecondary();
let secondaryDB = secondary.getDB("test");
let secondaryColl = secondaryDB.collate_id;

// The default WC is majority and rsSyncApplyStop failpoint will prevent satisfying any majority
// writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
// Stop the secondary from syncing. This will ensure that the writes on the primary get applied
// on the secondary in a large batch.
assert.commandWorked(secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
checkLog.contains(secondaryDB, "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");

assert.commandWorked(primaryDB.createCollection(primaryColl.getName(), caseInsensitive));

// A string of the character 'b' repeated.
const baseStr = "b".repeat(49);

for (let i = 0; i < 1000; i++) {
    // Make an _id by uppercasing each character in "baseStr" with 0.5 probability.
    let strId = baseStr;
    for (let charIdx = 0; charIdx < baseStr.length; charIdx++) {
        if (Random.rand() < 0.5) {
            strId = uppercaseIth(strId, charIdx);
        }
    }

    assert.commandWorked(primaryColl.insert({_id: strId}));
    assert.commandWorked(primaryColl.remove({_id: strId}));
}

// Since the inserts and deletes happen in pairs, we should be left with an empty collection on
// the primary.
assert.eq(0, primaryColl.find().itcount());

// Allow the secondary to sync, and test that it also ends up with an empty collection.
assert.commandWorked(secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
replTest.awaitReplication();
assert.eq(0, secondaryColl.find().itcount());
replTest.stopSet();
