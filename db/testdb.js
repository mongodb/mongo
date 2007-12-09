// testdb.js

var fail = 0;

var t = connect("test", "192.168.79.1");

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

function index2() { 
    t.z.remove({});
    t.z.save( { a: -3 } );
    t.z.ensureIndex( { a:true} );
    for( var i = 0; i < 300; i++ )
	t.z.save( { a: i, b: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccdddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddfffffffffffffffffffffffffffffff" } );
    t.z.remove({});
}

function bigIndexTest() { 
    t.big.remove({});
    t.big.save( { name: "Dwight" } );
    t.big.ensureIndex({name: true});
    for( var i = 0; i < 1000; i++ ) { 
	var x = { name: "e" + Math.random() + "abcdefasdflkjfdslkjdslkjfdslkjfdslkjfdslkjdflkj fdslkjfdslkjdsljfdslkjdsl fdslkfdslkjfdslkjlkjdsf fdslkjfds",
                  addr: "1234 main", phone: 7 };
	t.big.save(x);
    }
    for( var i = 0; i < 1000; i++ ) { 
	var x = { name: "c" + Math.random() + "abcdefasdflkjfdslkjdslkjfdslkjfdslkjfdslkjdflkj fdslkjfdslkjdsljfdslkjdsl fdslkfdslkjfdslkjlkjdsf fdslkjfds",
                  addr: "1234 main", phone: 7 };
	t.big.save(x);
    }
}

function runall() { 
    runquick();

    print("bigindextest stuff:");
    t.big.remove( { } );
    bigIndexTest();
    t.big.find().sort({name:true});
    t.big.remove( { } );
    t.big.find().sort({name:true});
    bigIndexTest();
    t.big.find().sort({name:true});
    t.big.remove( { } );
}

function testarrayindexing() { 
    print("testarrayindexing");
    t.ta.remove({});
    t.ta.save({name:"aaa", tags:["abc", "123", "foo"], z:1});
    t.ta.save({name:"baa", tags:["q", "123", 3], z:1});
    t.ta.save({name:"caa", tags:["dm", "123"], z:1});
    t.ta.save({name:"daa"});

    for( var pass=0; pass<=1; pass++ ) { 
	oneresult( t.ta.find({tags:"foo"}) );
	oneresult( t.ta.find({tags:3}) );
	assert( t.ta.find({tags:"123"}).length() == 3 );
	t.ta.ensureIndex({tags:true});
    }
}

function runquick() { 
    print("runquick");
    t.nullcheck.remove({});
    t.nullcheck.save( { a : 3 } );
    oneresult( t.nullcheck.find() ); 

    /* todo uncomment when eliot fixes! */
    assert( t.nullcheck.find({a:3})[0].a == 3, "a3" );
    oneresult( t.nullcheck.find( { b: null } ) ); progress();
    noresult( t.nullcheck.find( { b: 1 } ) ); progress();
    print("this one doesn't work yet todo:");
    oneresult( t.nullcheck.find( { a : "3" } ) ); progress();
    
    // regex
    print("regex");
    t.reg.remove({});
    t.reg.save( { name: "Dwight" } );
    for( i = 0; i < 2; i++ ) {
	oneresult( t.reg.find( { name: /Dwi./ } ), "re1" );
	noresult( t.reg.find( { name: /dwi./ } ), "re2" );
	oneresult( t.reg.find( { name: /dwi/i } ), "re3" );
	t.reg.ensureIndex( { name: true } );
    }
    
    testdelete();
    
    testarrayindexing();
}

print("testdb.js: try runall()");
print("               runquick()");
print("               bigIndexTest()");
