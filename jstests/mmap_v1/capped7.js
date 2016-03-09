// Test capped collection truncate via 'emptycapped' command

Random.setRandomSeed();

db.capped7.drop();
db._dbCommand({create: "capped7", capped: true, size: 1000, $nExtents: 11, autoIndexId: false});
tzz = db.capped7;

var ten = new Array(11).toString().replace(/,/g, "-");

count = 0;

/**
 * Insert new documents until the capped collection loops and the document
 * count doesn't increase on insert.
 */
function insertUntilFull() {
    count = tzz.count();
    var j = 0;
    while (1) {
        tzz.save({i: ten, j: j++});
        var newCount = tzz.count();
        if (count == newCount) {
            break;
        }
        count = newCount;
    }
}

insertUntilFull();

// oldCount == count before empty
oldCount = count;

assert.eq.automsg("11", "tzz.stats().numExtents");

// oldSize == size before empty
var oldSize = tzz.stats().storageSize;

assert.commandWorked(db._dbCommand({emptycapped: "capped7"}));

// check that collection storage parameters are the same after empty
assert.eq.automsg("11", "tzz.stats().numExtents");
assert.eq.automsg("oldSize", "tzz.stats().storageSize");

// check that the collection is empty after empty
assert.eq.automsg("0", "tzz.find().itcount()");
assert.eq.automsg("0", "tzz.count()");

// check that we can reuse the empty collection, inserting as many documents
// as we were able to the first time through.
insertUntilFull();
assert.eq.automsg("oldCount", "count");
assert.eq.automsg("oldCount", "tzz.find().itcount()");
assert.eq.automsg("oldCount", "tzz.count()");

assert.eq.automsg("11", "tzz.stats().numExtents");
var oldSize = tzz.stats().storageSize;

assert.commandWorked(db._dbCommand({emptycapped: "capped7"}));

// check that the collection storage parameters are unchanged after another empty
assert.eq.automsg("11", "tzz.stats().numExtents");
assert.eq.automsg("oldSize", "tzz.stats().storageSize");

// insert an arbitrary number of documents
var total = Random.randInt(2000);
for (var j = 1; j <= total; ++j) {
    tzz.save({i: ten, j: j});
    // occasionally check that only the oldest documents are removed to make room
    // for the newest documents
    if (Random.rand() > 0.95) {
        assert.automsg("j >= tzz.count()");
        assert.eq.automsg("tzz.count()", "tzz.find().itcount()");
        var c = tzz.find().sort({$natural: -1});
        var k = j;
        assert.automsg("c.hasNext()");
        while (c.hasNext()) {
            assert.eq.automsg("c.next().j", "k--");
        }
        // check the same thing with a reverse iterator as well
        var c = tzz.find().sort({$natural: 1});
        assert.automsg("c.hasNext()");
        while (c.hasNext()) {
            assert.eq.automsg("c.next().j", "++k");
        }
        assert.eq.automsg("j", "k");
    }
}
