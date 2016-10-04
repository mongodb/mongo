/**
 * Test for SERVER-2417 - should not preallocate a database file while we are
 * dropping its directory in directoryperdb mode.
 */

var baseDir = "jstests_disk_preallocate_directoryperdb";
var baseName = "preallocate_directoryperdb";
var baseName2 = "preallocate_directoryperdb2";
var baseName3 = "preallocate_directoryperdb3";
dbpath = MongoRunner.dataPath + baseDir + "/";

function checkDb2DirAbsent() {
    files = listFiles(dbpath);
    //    printjson( files );
    for (var f in files) {
        var name = files[f].name;
        assert.eq(-1, name.indexOf(dbpath + baseName2), "baseName2 dir still present");
    }
}

var m = MongoRunner.runMongod(
    {smallfiles: "", directoryperdb: "", dbpath: dbpath, bind_ip: "127.0.0.1"});
db = m.getDB(baseName);
db2 = m.getDB(baseName2);
var bulk = db[baseName].initializeUnorderedBulkOp();
var bulk2 = db2[baseName2].initializeUnorderedBulkOp();
var big = new Array(5000).toString();
for (var i = 0; i < 3000; ++i) {
    bulk.insert({b: big});
    bulk2.insert({b: big});
}
assert.writeOK(bulk.execute());
assert.writeOK(bulk2.execute());

// Due to our write pattern, we expect db2's .3 file to be queued up in the file
// allocator behind db's .3 file at the time db2 is dropped.  This will
// (incorrectly) cause db2's dir to be recreated until SERVER-2417 is fixed.
db2.dropDatabase();

checkDb2DirAbsent();

db.dropDatabase();

// Try writing a new database, to ensure file allocator is still working.
db3 = m.getDB(baseName3);
c3 = db[baseName3];
assert.writeOK(c3.insert({}));
assert.eq(1, c3.count());

checkDb2DirAbsent();
