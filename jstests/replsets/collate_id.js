// Test that oplog application on the secondary happens correctly when the collection has a default
// collation and operations by _id which must respect the collation are issued.
(function() {
    "use strict";

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

    const caseInsensitive = {collation: {locale: "en_US", strength: 2}};

    var replTest = new ReplSetTest({name: 'testSet', nodes: 2});
    var nodes = replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var primaryDB = primary.getDB("test");
    var primaryColl = primaryDB.collate_id;

    var secondary = replTest.getSecondary();
    var secondaryDB = secondary.getDB("test");
    var secondaryColl = secondaryDB.collate_id;

    // Stop the secondary from syncing. This will ensure that the writes on the primary get applied
    // on the secondary in a large batch.
    assert.commandWorked(
        secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));

    assert.commandWorked(primaryDB.createCollection(primaryColl.getName(), caseInsensitive));

    // A string of the character 'b' repeated.
    const baseStr = new Array(50).join("b");

    for (var i = 0; i < 1000; i++) {
        // Make an _id by uppercasing each character in "baseStr" with 0.5 probability.
        var strId = baseStr;
        for (var charIdx = 0; charIdx < baseStr.length; charIdx++) {
            if (Random.rand() < 0.5) {
                strId = uppercaseIth(strId, charIdx);
            }
        }

        assert.writeOK(primaryColl.insert({_id: strId}));
        assert.writeOK(primaryColl.remove({_id: strId}));
    }

    // Since the inserts and deletes happen in pairs, we should be left with an empty collection on
    // the primary.
    assert.eq(0, primaryColl.find().itcount());

    // Allow the secondary to sync, and test that it also ends up with an empty collection.
    assert.commandWorked(
        secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
    replTest.awaitReplication();
    assert.eq(0, secondaryColl.find().itcount());
})();
