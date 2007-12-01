// testdb.js

var fail = 0;

var t = connect("test");

function failure(f) { 
 print("FAIL: " + f);
 fail++;
}

function assert(x) { 
 if( !x )
  failure("assert");
}

function runall() { 

 t.nullcheck.save( { a : 3 } );
 oneresult( t.nullcheck.find() );
 assert( t.nullcheck.find({a:3})[0].a == 3 );
 oneresult( t.nullcheck.find( { b: null } ) );
 noresult( t.nullcheck.find( { b: 1 } ) );
 oneresult( t.nullcheck.find( { a : "3" } ) );

 // regex
 t.reg.save( { name: "Dwight" } );
 for( i = 0; i < 2; i++ ) {
  oneresult( t.reg.find( { name: /Dwi./ } ) );
  noresult( t.reg.find( { name: /dwi./ } ) );
  oneresult( t.reg.find( { name: /dwi/i } ) );
  t.reg.ensureIndex( { name: true } );
 }


}



