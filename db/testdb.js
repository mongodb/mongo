// testdb.js

var fail = 0;

var t = connect("test", "192.168.37.1");

var z = 0;
function progress() {}// print(++z); }

function failure(f, args) { 
 print("FAIL: " + f);
 fail++;
 if( args.length >= 2 )
     print(args[1]);
}

function assert(x) {
    if( !x )
	failure("assert", arguments);
}

function oneresult(c) { 
    if( c.length() != 1 ) {
	failure("ERROR: wrong # of results: " + c.length(), arguments);
    }
}

function noresult(c) { 
    if( c.length() != 0 )
	failure("ERROR: wrong # of results: " + c.length(), arguments);
}

function testkeys() { 
    t.testkeys.save( { name: 5 } );
    t.testkeys.ensureIndex({name:true});
    t.testkeys.save( { name: 6 } );
    t.testkeys.save( { name: 8 } );
    t.testkeys.save( { name: 3 } );
    print("t.testkeys");
}

function testdelete() { 
    print("testdelete");
    t.testkeys.remove({});
    testkeys();
    t.testkeys.remove({});
    testkeys();
    assert( t.testkeys.find().toArray().length == 4, "testkeys" );
}

function runall() { 
    print("runall");
    t.nullcheck.remove({});
 t.nullcheck.save( { a : 3 } );
 oneresult( t.nullcheck.find() ); progress();
 print("runall 1");

 assert( t.nullcheck.find({a:3})[0].a == 3, "a3" );
 oneresult( t.nullcheck.find( { b: null } ) ); progress();
 noresult( t.nullcheck.find( { b: 1 } ) ); progress();
 oneresult( t.nullcheck.find( { a : "3" } ) ); progress();


 // regex
 print("regex");
 t.reg.save( { name: "Dwight" } );
 for( i = 0; i < 2; i++ ) {
     oneresult( t.reg.find( { name: /Dwi./ } ), "re1" );
     noresult( t.reg.find( { name: /dwi./ } ), "re2" );
     oneresult( t.reg.find( { name: /dwi/i } ), "re3" );
     t.reg.ensureIndex( { name: true } );
 }

 testdelete();

}

print("testdb.js: try runall()");
