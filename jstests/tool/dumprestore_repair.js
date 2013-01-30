/* dumprestore_repair.js
 * create collection that spans more than one extent.
 * mongodump using both --repair and normal
 * restore both dumps and assert they're equal
 * assert that the --repair dump is 2 times the size of the non --repair dump
 */
t = new ToolTest( "dumprestore_repair" );
c = t.startDB( "foo" );
dbName = t.db;
assert.eq( 0 , c.count() , "foo" );
for (i=0; i<100; i++) { c.save( { x : i } ); }
assert.eq( 100 , c.count() , "foo" );
t.stop();

// normal
normalDumpPath = t.ext + 'normal'
t.runTool( "dump",  "--dbpath", t.dbpath, "-d",  t.baseName, "-c", "foo", "--out", normalDumpPath );

// with repair
repairDumpPath = t.ext + 'repair'
t.runTool( "dump", "--repair", "--dbpath", t.dbpath, "-d",  t.baseName, "-c", "foo", "--out", repairDumpPath );

c = t.startDB( "foo" );

function restore(path, toolTest, coll) {
    coll.drop();
    assert.eq( 0 , coll.count() , "after drop" );
    toolTest.runTool( "restore" , "--dir" , path );
    assert.soon( "c.findOne()" , "no data after sleep" );
    assert.eq( 100 , c.count() , "after restore" );
}

restore(normalDumpPath, t, c);
restore(repairDumpPath, t, c);

// get the dumped bson files
normalFiles = listFiles( normalDumpPath + '/'  + t.baseName )

// filter out the metadata.json file
normalFiles = normalFiles.filter( function(x) { if ( x.name.match( /bson$/ ) ) return x; } )
assert.eq( normalFiles[0].name, normalDumpPath + "/" + t.baseName + "/foo.bson", "unexpected file name")
repairFiles = listFiles( repairDumpPath + '/'  + t.baseName )
assert.eq( repairFiles[0].name, repairDumpPath + "/" + t.baseName + "/foo.bson", "unexpected file name")

// the --repair bson file should be exactly twice the size of the normal dump file
assert.eq( normalFiles[0].size * 2, repairFiles[0].size );
t.stop();
