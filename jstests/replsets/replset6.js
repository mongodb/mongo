
// Test replication of collection renaming

baseName = "jstests_replsets_replset6";

var rt = new ReplSetTest({ name : "replset6tests" , nodes: 2 });
var nodes = rt.startSet();
rt.initiate();
var m = rt.getMaster();
rt.awaitSecondaryNodes();
var slaves = rt.liveNodes.slaves;
s = slaves[0];
s.setSlaveOk();
admin = m.getDB( "admin" );

debug = function( foo ) {} // print( foo ); }

// rename within db

m.getDB( baseName ).one.save( { a: 1 } );
assert.soon( function() { v = s.getDB( baseName ).one.findOne(); return v && 1 == v.a } );

assert.commandWorked( admin.runCommand( {renameCollection:"jstests_replsets_replset6.one", to:"jstests_replsets_replset6.two"} ) );
assert.soon( function() {
	if ( -1 == s.getDB( baseName ).getCollectionNames().indexOf( "two" ) ) {
	    debug( "no two coll" );
	    debug( tojson( s.getDB( baseName ).getCollectionNames() ) );
	    return false;
	}
	if ( !s.getDB( baseName ).two.findOne() ) {
	    debug( "no two object" );
	    return false;
	}
	    return 1 == s.getDB( baseName ).two.findOne().a; });
assert.eq( -1, s.getDB( baseName ).getCollectionNames().indexOf( "one" ) );

// rename to new db

first = baseName + "_first";
second = baseName + "_second";

m.getDB( first ).one.save( { a: 1 } );
assert.soon( function() { return s.getDB( first ).one.findOne() && 1 == s.getDB( first ).one.findOne().a; } );

assert.commandWorked( admin.runCommand( {renameCollection:"jstests_replsets_replset6_first.one", to:"jstests_replsets_replset6_second.two"} ) );
assert.soon( function() {
	return -1 != s.getDBNames().indexOf( second ) &&
	    -1 != s.getDB( second ).getCollectionNames().indexOf( "two" ) &&
	    s.getDB( second ).two.findOne() &&
	    1 == s.getDB( second ).two.findOne().a; } );
assert.eq( -1, s.getDB( first ).getCollectionNames().indexOf( "one" ) );

