t = db.jstests_rename4;
t.drop();

function c( f ) {
    assert( !db.getLastError(), "error" );
    eval( f );
    assert( db.getLastError(), "no error" );
    db.resetError();
}

c( "t.update( {}, {$rename:{'a':'a'}} )" );
c( "t.update( {}, {$rename:{'':'a'}} )" );
c( "t.update( {}, {$rename:{'a':''}} )" );
c( "t.update( {}, {$rename:{'_id':'a'}} )" );
c( "t.update( {}, {$rename:{'a':'_id'}} )" );
c( "t.update( {}, {$rename:{'_id.a':'b'}} )" );
c( "t.update( {}, {$rename:{'b':'_id.a'}} )" );
c( "t.update( {}, {$rename:{'_id.a':'_id.b'}} )" );
c( "t.update( {}, {$rename:{'_id.b':'_id.a'}} )" );
c( "t.update( {}, {$rename:{'.a':'b'}} )" );
c( "t.update( {}, {$rename:{'a':'.b'}} )" );
c( "t.update( {}, {$rename:{'a.':'b'}} )" );
c( "t.update( {}, {$rename:{'a':'b.'}} )" );
c( "t.update( {}, {$rename:{'a.b':'a'}} )" );
c( "t.update( {}, {$rename:{'a.$':'b'}} )" );
c( "t.update( {}, {$rename:{'a':'b.$'}} )" );
c( "t.update( {}, {$set:{b:1},$rename:{'a':'b'}} )" );
c( "t.update( {}, {$rename:{'a':'b'},$set:{b:1}} )" );
c( "t.update( {}, {$rename:{'a':'b'},$set:{a:1}} )" );
c( "t.update( {}, {$set:{'b.c':1},$rename:{'a':'b'}} )" );
c( "t.update( {}, {$set:{b:1},$rename:{'a':'b.c'}} )" );
c( "t.update( {}, {$rename:{'a':'b'},$set:{'b.c':1}} )" );
c( "t.update( {}, {$rename:{'a':'b.c'},$set:{b:1}} )" );

t.save( {a:[1],b:{c:[1]},d:[{e:1}],f:1} );
c( "t.update( {}, {$rename:{'a.0':'f'}} )" );
c( "t.update( {}, {$rename:{'a.0':'g'}} )" );
c( "t.update( {}, {$rename:{'f':'a.0'}} )" );
c( "t.update( {}, {$rename:{'b.c.0':'f'}} )" );
c( "t.update( {}, {$rename:{'f':'b.c.0'}} )" );
c( "t.update( {}, {$rename:{'d.e':'d.f'}} )" );
c( "t.update( {}, {$rename:{'d.e':'f'}} )" );
c( "t.update( {}, {$rename:{'d.f':'d.e'}} )" );
c( "t.update( {}, {$rename:{'f':'d.e'}} )" );
c( "t.update( {}, {$rename:{'d.0.e':'d.f'}} )" );
c( "t.update( {}, {$rename:{'d.0.e':'f'}} )" );
c( "t.update( {}, {$rename:{'d.f':'d.0.e'}} )" );
c( "t.update( {}, {$rename:{'f':'d.0.e'}} )" );
c( "t.update( {}, {$rename:{'f.g':'a'}} )" );
c( "t.update( {}, {$rename:{'a':'f.g'}} )" );

function v( start, mod, expected ) {
    t.remove();
    t.save( start );
    t.update( {}, mod );
    assert( !db.getLastError() );
    var got = t.findOne();
    delete got._id;
    assert.eq( expected, got );
}

v( {a:1}, {$rename:{a:'b'}}, {b:1} );
v( {a:1}, {$rename:{a:'bb'}}, {bb:1} );
v( {b:1}, {$rename:{b:'a'}}, {a:1} );
v( {bb:1}, {$rename:{bb:'a'}}, {a:1} );
v( {a:{y:1}}, {$rename:{'a.y':'a.z'}}, {a:{z:1}} );
v( {a:{yy:1}}, {$rename:{'a.yy':'a.z'}}, {a:{z:1}} );
v( {a:{z:1}}, {$rename:{'a.z':'a.y'}}, {a:{y:1}} );
v( {a:{zz:1}}, {$rename:{'a.zz':'a.y'}}, {a:{y:1}} );
v( {a:{c:1}}, {$rename:{a:'b'}}, {b:{c:1}} );
v( {aa:{c:1}}, {$rename:{aa:'b'}}, {b:{c:1}} );
v( {a:1,b:2}, {$rename:{a:'b'}}, {b:1} );
v( {aa:1,b:2}, {$rename:{aa:'b'}}, {b:1} );
v( {a:1,bb:2}, {$rename:{a:'bb'}}, {bb:1} );
v( {a:1}, {$rename:{a:'b.c'}}, {b:{c:1}} );
v( {aa:1}, {$rename:{aa:'b.c'}}, {b:{c:1}} );
v( {a:1,b:{}}, {$rename:{a:'b.c'}}, {b:{c:1}} );
v( {aa:1,b:{}}, {$rename:{aa:'b.c'}}, {b:{c:1}} );
v( {a:1}, {$rename:{b:'c'}}, {a:1} );
v( {aa:1}, {$rename:{b:'c'}}, {aa:1} );
v( {}, {$rename:{b:'c'}}, {} );
v( {a:{b:1,c:2}}, {$rename:{'a.b':'d'}}, {a:{c:2},d:1} );
v( {a:{bb:1,c:2}}, {$rename:{'a.bb':'d'}}, {a:{c:2},d:1} );
v( {a:{b:1}}, {$rename:{'a.b':'d'}}, {a:{},d:1} );
v( {a:[5]}, {$rename:{a:'b'}}, {b:[5]} );
v( {aa:[5]}, {$rename:{aa:'b'}}, {b:[5]} );
v( {'0':1}, {$rename:{'0':'5'}}, {'5':1} );
v( {a:1,b:2}, {$rename:{a:'c'},$set:{b:5}}, {b:5,c:1} );
v( {aa:1,b:2}, {$rename:{aa:'c'},$set:{b:5}}, {b:5,c:1} );
v( {a:1,b:2}, {$rename:{z:'c'},$set:{b:5}}, {a:1,b:5} );
v( {aa:1,b:2}, {$rename:{z:'c'},$set:{b:5}}, {aa:1,b:5} );

// (formerly) rewriting single field
v( {a:{z:1,b:1}}, {$rename:{'a.b':'a.c'}}, {a:{c:1,z:1}} );
v( {a:{z:1,tomato:1}}, {$rename:{'a.tomato':'a.potato'}}, {a:{potato:1,z:1}} );
v( {a:{z:1,b:1,c:1}}, {$rename:{'a.b':'a.c'}}, {a:{c:1,z:1}} );
v( {a:{z:1,tomato:1,potato:1}}, {$rename:{'a.tomato':'a.potato'}}, {a:{potato:1,z:1}} );
v( {a:{z:1,b:1}}, {$rename:{'a.b':'a.cc'}}, {a:{cc:1,z:1}} );
v( {a:{z:1,b:1,c:1}}, {$rename:{'a.b':'aa.c'}}, {a:{c:1,z:1},aa:{c:1}} );

// invalid target, but missing source
v( {a:1,c:4}, {$rename:{b:'c.d'}}, {a:1,c:4} );

// check index
t.drop();
t.ensureIndex( {a:1} );

function l( start, mod, query, expected ) {
    t.remove();
    t.save( start );
    t.update( {}, mod );
    assert( !db.getLastError() );
    var got = t.find( query ).hint( {a:1} ).next();
    delete got._id;
    assert.eq( expected, got );
}

l( {a:1}, {$rename:{a:'b'}}, {a:null}, {b:1} );
l( {a:1}, {$rename:{a:'bb'}}, {a:null}, {bb:1} );
l( {b:1}, {$rename:{b:'a'}}, {a:1}, {a:1} );
l( {bb:1}, {$rename:{bb:'a'}}, {a:1}, {a:1} );
