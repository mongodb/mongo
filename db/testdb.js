// testdb.js

var fail = 0;

var t = connect("test");
db=t;

core.db.db();

var z = 0;
function progress() {}// print(++z); }

function failure(f, args) { 
    print("FAIL: " + f + ' ' + (args.length<2?"":args[1]));
    fail++;
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

function testdots() { 
    t.dots.remove({});
    t.dots.save( { a: 3, b: { y: 4, z : 5 } } );
    oneresult( t.dots.find( { a:3 } ) );
    oneresult( t.dots.find( { b: { y:4,z:5} } ) );
    oneresult( t.dots.find( { a:3, b: { y:4,z:5} } ) );
    noresult( t.dots.find( { b: { y:4} } ) );
    oneresult( t.dots.find( { "b.y":4 } ) );
    oneresult( t.dots.find( { "b.z":5 } ) );
    noresult( t.dots.find( { "b.z":55 } ) );
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

function giantIndexTest() { 
    print("giantIndexTest");
    drop("giant");
    db.giant.save({moe:1,foo:[33],bar:"aaaaa"});

    var z = 0;
    var prime = 127;
    for( var i = 0; i < 20000; i++ ) { 
	var ar = [];
	for( var j = 0; j < 100; j++ ) { 
	    z += prime;
	    ar.push( z % 100 );
	}
	db.giant.save({foo:ar, bar:"bbbbb"});
	if( i % 1000 == 0 ) print(i);
	if( i == 10000 )
	    db.giant.ensureIndex({foo:1});
    }


    assert( db.giant.findOne({foo:33}) );
    print("giantIndexTest end");
}

function giant2() { 
    print("giant2");
    drop("giant");
    db.giant.save({moe:1,foo:[33],bar:"aaaaa",q:-1});

    var z = 0;
    var prime = 127;
    for( var i = 0; i < 20000; i++ ) { 
	var ar = [];
	for( var j = 0; j < 100; j++ ) { 
	    z += prime;
	    ar.push( z % 100 );
	}
	db.giant.save({foo:ar, bar:"bbbbb", q:i});
	if( i % 1000 == 0 ) print(i);
	if( 0 && i == 10000 )
	    db.giant.ensureIndex({foo:1});
    }


    assert( db.giant.findOne({foo:33}) );
    print("giant2  end");
}

// mx=-3; 
// db.giant.find().forEach( function(x) { 
//   if( x.q % 100 == 0 ) print(x.q); if( x.q > mx ) { x.name = "john smith"; db.giant.save(x); mx = x.q; } } );} } );  

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
    runcursors();

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

function testcapped(max) { 
    print("testcapped");
    drop("capped");

    assert( createCollection("capped", { size: 4096, capped:true, max:max } ).ok );

    capped = db.capped;
    for(i=0; i<500; i++ ) { 
	capped.save( { i: i, b: "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxyyyyyyyyyyyyyyyyyyy" } );
    }

    var a = capped.find().toArray();
    assert( a.length < 100 );
    assert( a[a.length-1].i == 499 );
    assert( capped.find().sort({$natural:-1}).limit(1)[0].i == 499 );
    print("testcapped end");
}

function testgetmore() { 
    print("testgetmore");
    drop("gm");
    gm=t.gm;
    for(i=0;i<50000;i++){
	gm.save({a:i, b:"adsffffffffffffffffffffffffffffffffffffffffffffffff\nfffffffffffffffffffffffffffffffffffffffffffffffffffff\nfffffffffffffffffffffffffffffffff"})
	    }
    assert(gm.find().length()==50000);

    x = 0;
    c=gm.find();
    for(i=0;i<5000;i++) { x += c.next().a; }

    assert(gm.find().length()==50000); // full iteration with a cursor already live
    assert( gm.find()[10000].a == 10000 );
    assert( gm.find()[29000].a == 29000 );
    assert( gm.find()[9000].a == 9000 );
    d=gm.find();
    assert( d[20000].a==20000 );
    assert( d[10000].a==10000 );
    assert( d[40000].a==40000 );
    assert(gm.find().length()==50000); // full iteration with a cursor already live

    print( connect("intr").cursors.findOne().dump );

    print("testgetmore end");
}

function testdups() { 
 print("testdups");
 K = 2000;
 for( pass=0;pass<1;pass++ ) {
     print(" pass:" + pass);
     if( pass < 2 ) {
	 print("removing keys");
	 t.td.remove({});
     }
     print("add keys");
     for( var x=0;x<K;x++ )
	 t.td.save({ggg:"asdfasdf bbb a a jdssjsjdjds dsdsdsdsds d", z: x, 
		     str: "a long string dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"});
     assert( t.td.find({ggg:"asdfasdf bbb a a jdssjsjdjds dsdsdsdsds d"}).toArray().length == K );
     //     t.td.ensureIndex({ggg:true});
     if( pass == 0 )
	 t.td.ensureIndex({ggg:1});
     //     else if( pass == 1 ) 
     //	 t.td.ensureIndex({ggg:-1});
 }
 print(" end testdups");
 print(" try t.td.remove({});");
}

function testdups2() { 
 print("testdups");
 for( pass=0;pass<1;pass++ ) {
     print(" pass:" + pass);
     t.td.remove({});
     for( var x=0;x<250;x++ )
	 t.td.save({ggg:"asdfasdf bbb a a jdssjsjdjds dsdsdsdsds d", z: x, 
		     str: "a long string dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"});
     assert( t.td.find({ggg:"asdfasdf bbb a a jdssjsjdjds dsdsdsdsds d"}).toArray().length == 2000 );
     t.td.ensureIndex({ggg:true});
 }
 t.td.remove({});
 print(" end testdups");
}


/*
 *   tests UTF-8 in the regexp package in the db : save a string w/ two unicode characters
 *   and then pass a regex that looks for a match of both chars.   If regex on db is borked
 *   we'll be matching against the two "low order" characters, rather than the two double-byte
 *   characters
 */
function test_utf8() {
    db.utf.save({str:"123abc\u0253\u0253"});
    assert(db.utf.findOne({str:RegExp("\u0253{2,2}")}));
    db.utf.remove({});
}

function runcursors() {
    print("initial remove");
    t.cur.remove({});
    t.cur.findOne();
    print(" done");

    print( tojson( connect("intr").cursors.find() ) );

    print("insert");

    for( i = 0; i < 50000; i++ )
	t.cur.save( { name:"ABC", k:/asfd/, a:i, 
		    lng:"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		    lng1:"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		    lng2:"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"} );

    for( i = 0; i < 100; i++ ) {
	c = t.cur.find().limit(2);
	print(c[0].name);
	t.cur.find({name:"ABC"}).limit(3)[0];
    }

    print( tojson( connect("intr").cursors.find() ) );

    print("Remove...");
    t.cur.remove({});
    t.cur.findOne();
    print(" done");

    print( tojson( connect("intr").cursors.find() ) );

    print(" end runcursors");
}

function runquick() { 
    print("runquick");
    start = Date();

    testdots();

    testcapped();

    testgetmore();

    t.nullcheck.remove({});
    t.nullcheck.save( { a : 3 } );
    oneresult( t.nullcheck.find() ); 

    /* todo uncomment when eliot fixes! */
    assert( t.nullcheck.find({a:3})[0].a == 3, "a3" );
    oneresult( t.nullcheck.find( { b: null } ) ); progress();
    noresult( t.nullcheck.find( { b: 1 } ) ); progress();
    oneresult( t.nullcheck.find( { a : "3" } ), "todo num to str match" ); progress();
    
    // regex
    print("regex");
    t.reg.remove({});
    t.reg.save( { name: "Dwight", a : 345, dt: Date() } );
    for( i = 0; i < 2; i++ ) {
	oneresult( t.reg.find( { name: /Dwi./ } ), "re1" );
	oneresult( t.reg.find( { dt: /20/ } ), "date regexp match" );
	oneresult( t.reg.find( { a: /34/ } ), "regexp match number" );
	noresult( t.reg.find( { name: /dwi./ } ), "re2" );
	oneresult( t.reg.find( { name: /dwi/i } ), "re3" );
	t.reg.ensureIndex( { name: true } );
    }
    
    testdelete();
    
    testarrayindexing();

    runcursors();

    print("Testing UTF-8 support in db regex");
    test_utf8();

    print("testdups last to go, it takes a little time...");
    testdups();

    print("runquick done");
    print("start: " + start);
    print("finish: " + Date());
}

print("testdb.js: try runall()");
print("               runquick()");
print("               bigIndexTest()");
print("               giantindexTest()");
print("               runcursors()");
print("               testgetmore()");
print("               testcapped()");

