
// test for SERVER-6303 - if documents move backward during an initial sync.

var rt = new ReplSetTest( { name : "replset8tests" , nodes: 1 } );

var nodes = rt.startSet();
rt.initiate();
var master = rt.getMaster();
var bigstring = "a";
var md = master.getDB( 'd' );
var mdc = md[ 'c' ];

// prep the data

// idea: create x documents of increasing size, then create x documents of size n.
//       delete first x documents.  start initial sync (cloner).  update all remaining 
//       documents to be increasing size.
//       this should result in the updates moving the docs backwards.

var doccount = 10000;
// Avoid empty extent issues
mdc.insert( { _id:-1, x:"dummy" } );

print ("inserting bigstrings");
for( i = 0; i < doccount; ++i ) {
    mdc.insert( { _id:i, x:bigstring } );
    bigstring += "a";
}
md.getLastError();

print ("inserting x");
for( i = doccount; i < doccount*2; ++i ) {
    mdc.insert( { _id:i, x:i } );
}
md.getLastError();

print ("deleting bigstrings");
for( i = 0; i < doccount; ++i ) {
    mdc.remove( { _id:i } );
}
md.getLastError();

// add a secondary
var slave = rt.add();
rt.reInitiate();
print ("initiation complete!");
var sc = slave.getDB( 'd' )[ 'c' ];
slave.setSlaveOk();
sleep(25000);
print ("updating documents backwards");
// Move all documents to the beginning by growing them to sizes that should
// fit the holes we made in phase 1
for (i = doccount*2; i > doccount; --i) {
    mdc.update( { _id:i, x:i }, { _id:i, x:bigstring } );
    md.getLastError();
    bigstring = bigstring.slice(0, -1); // remove last char
}
print ("finished");
// Wait for replication to catch up.
rt.awaitSecondaryNodes();
assert.eq(doccount+1, slave.getDB( 'd' )['c'].count());
