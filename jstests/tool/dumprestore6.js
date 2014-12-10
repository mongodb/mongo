// dumprestore6.js
// Test restoring from a dump with an old index version


t = new ToolTest( "dumprestore6" );

c = t.startDB( "foo" );
db = t.db
assert.eq( 0 , c.count() , "setup1" );

t.runTool("restore", "--dir", "jstests/tool/data/dumprestore6", "--db", "jstests_tool_dumprestore6")

assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore" );

var indexes = c.getIndexes();
assert.eq( 2, indexes.length, "there aren't the correct number of indexes" );
var aIndex = null;
indexes.forEach(function(index) {
    if (index.name === "a_1") {
        aIndex = index;
    }
});
assert.neq(null, aIndex, "index doesn't exist" );
assert.eq( 1 , aIndex.v, "index version wasn't updated");

assert.eq( 1, c.count({v:0}), "dropped the 'v' field from a non-index collection")

db.dropDatabase()
assert.eq( 0 , c.count() , "after drop" );

t.runTool("restore", "--dir", "jstests/tool/data/dumprestore6", "--db", "jstests_tool_dumprestore6", "--keepIndexVersion")

assert.soon( "c.findOne()" , "no data after sleep2" );
assert.eq( 1 , c.count() , "after restore2" );

indexes = c.getIndexes();
assert.eq( 2, indexes.length, "there aren't the correct number of indexes" );
aIndex = null;
indexes.forEach(function(index) {
    if (index.name === "a_1") {
        aIndex = index;
    }
});
assert.neq(null, aIndex, "index doesn't exist" );
assert.eq( 0 , aIndex.v, "index version wasn't maintained")

assert.eq( 1, c.count({v:0}), "dropped the 'v' field from a non-index collection")

t.stop();
