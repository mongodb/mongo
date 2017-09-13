// SERVER-13912 Capped collections with size=0 are promoted to the minimum Extent size
var name = "capped_server13912";
var minExtentSize = 0x1000;  // from ExtentManager::minSize()

var t = db.getCollection(name);
t.drop();

db.createCollection(name, {capped: true, size: 0});

assert.eq(t.stats().storageSize, minExtentSize);
