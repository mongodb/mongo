t = db.jstests_rename4;
t.drop();

function bad( f ) {
    //Ensure no error to start with
    var lstError = db.getLastError();
    if (lstError)
        assert( false, "Unexpected error : " + lstError );

    var docsBeforeUpdate = t.find().toArray();
    eval( f );

    //Ensure error
    var lstError = db.getLastErrorObj();
    if (!lstError.err) {
        print("Error:" + tojson(lstError));
        print("Existing docs (before)")
        printjson(docsBeforeUpdate);
        print("Existing docs (after)")
        printjson(t.find().toArray());
        assert( false, "Expected error but didn't get one for: " + f );
    }

    db.resetError();
}

bad( "t.update( {}, {$rename:{'a':'a'}} )" );
bad( "t.update( {}, {$rename:{'':'a'}} )" );
bad( "t.update( {}, {$rename:{'a':''}} )" );
bad( "t.update( {}, {$rename:{'_id':'a'}} )" );
bad( "t.update( {}, {$rename:{'a':'_id'}} )" );
bad( "t.update( {}, {$rename:{'_id.a':'b'}} )" );
bad( "t.update( {}, {$rename:{'b':'_id.a'}} )" );
bad( "t.update( {}, {$rename:{'_id.a':'_id.b'}} )" );
bad( "t.update( {}, {$rename:{'_id.b':'_id.a'}} )" );
bad( "t.update( {}, {$rename:{'.a':'b'}} )" );
bad( "t.update( {}, {$rename:{'a':'.b'}} )" );
bad( "t.update( {}, {$rename:{'a.':'b'}} )" );
bad( "t.update( {}, {$rename:{'a':'b.'}} )" );
bad( "t.update( {}, {$rename:{'a.b':'a'}} )" );
bad( "t.update( {}, {$rename:{'a.$':'b'}} )" );
bad( "t.update( {}, {$rename:{'a':'b.$'}} )" );

// Only bad if input doc has field resulting in conflict
t.save( {_id:1, a:1} );
bad( "t.update( {}, {$set:{b:1},$rename:{'a':'b'}} )" );
bad( "t.update( {}, {$rename:{'a':'b'},$set:{b:1}} )" );
bad( "t.update( {}, {$rename:{'a':'b'},$set:{a:1}} )" );
bad( "t.update( {}, {$set:{'b.c':1},$rename:{'a':'b'}} )" );
bad( "t.update( {}, {$set:{b:1},$rename:{'a':'b.c'}} )" );
bad( "t.update( {}, {$rename:{'a':'b'},$set:{'b.c':1}} )" );
bad( "t.update( {}, {$rename:{'a':'b.c'},$set:{b:1}} )" );


t.remove({});
t.save( {a:[1],b:{c:[1]},d:[{e:1}],f:1} );
bad( "t.update( {}, {$rename:{'a.0':'f'}} )" );
bad( "t.update( {}, {$rename:{'a.0':'g'}} )" );
bad( "t.update( {}, {$rename:{'f':'a.0'}} )" );
bad( "t.update( {}, {$rename:{'b.c.0':'f'}} )" );
bad( "t.update( {}, {$rename:{'f':'b.c.0'}} )" );
bad( "t.update( {}, {$rename:{'d.e':'d.f'}} )" );
bad( "t.update( {}, {$rename:{'d.e':'f'}} )" );
bad( "t.update( {}, {$rename:{'d.f':'d.e'}} )" );
bad( "t.update( {}, {$rename:{'f':'d.e'}} )" );
bad( "t.update( {}, {$rename:{'d.0.e':'d.f'}} )" );
bad( "t.update( {}, {$rename:{'d.0.e':'f'}} )" );
bad( "t.update( {}, {$rename:{'d.f':'d.0.e'}} )" );
bad( "t.update( {}, {$rename:{'f':'d.0.e'}} )" );
bad( "t.update( {}, {$rename:{'f.g':'a'}} )" );
bad( "t.update( {}, {$rename:{'a':'f.g'}} )" );

function good( start, mod, expected ) {
    t.remove();
    t.save( start );
    t.update( {}, mod );
    assert( !db.getLastError() );
    var got = t.findOne();
    delete got._id;
    assert.docEq( expected, got );
}

good( {a:1}, {$rename:{a:'b'}}, {b:1} );
good( {a:1}, {$rename:{a:'bb'}}, {bb:1} );
good( {b:1}, {$rename:{b:'a'}}, {a:1} );
good( {bb:1}, {$rename:{bb:'a'}}, {a:1} );
good( {a:{y:1}}, {$rename:{'a.y':'a.z'}}, {a:{z:1}} );
good( {a:{yy:1}}, {$rename:{'a.yy':'a.z'}}, {a:{z:1}} );
good( {a:{z:1}}, {$rename:{'a.z':'a.y'}}, {a:{y:1}} );
good( {a:{zz:1}}, {$rename:{'a.zz':'a.y'}}, {a:{y:1}} );
good( {a:{c:1}}, {$rename:{a:'b'}}, {b:{c:1}} );
good( {aa:{c:1}}, {$rename:{aa:'b'}}, {b:{c:1}} );
good( {a:1,b:2}, {$rename:{a:'b'}}, {b:1} );
good( {aa:1,b:2}, {$rename:{aa:'b'}}, {b:1} );
good( {a:1,bb:2}, {$rename:{a:'bb'}}, {bb:1} );
good( {a:1}, {$rename:{a:'b.c'}}, {b:{c:1}} );
good( {aa:1}, {$rename:{aa:'b.c'}}, {b:{c:1}} );
good( {a:1,b:{}}, {$rename:{a:'b.c'}}, {b:{c:1}} );
good( {aa:1,b:{}}, {$rename:{aa:'b.c'}}, {b:{c:1}} );
good( {a:1}, {$rename:{b:'c'}}, {a:1} );
good( {aa:1}, {$rename:{b:'c'}}, {aa:1} );
good( {}, {$rename:{b:'c'}}, {} );
good( {a:{b:1,c:2}}, {$rename:{'a.b':'d'}}, {a:{c:2},d:1} );
good( {a:{bb:1,c:2}}, {$rename:{'a.bb':'d'}}, {a:{c:2},d:1} );
good( {a:{b:1}}, {$rename:{'a.b':'d'}}, {a:{},d:1} );
good( {a:[5]}, {$rename:{a:'b'}}, {b:[5]} );
good( {aa:[5]}, {$rename:{aa:'b'}}, {b:[5]} );
good( {'0':1}, {$rename:{'0':'5'}}, {'5':1} );
good( {a:1,b:2}, {$rename:{a:'c'},$set:{b:5}}, {b:5,c:1} );
good( {aa:1,b:2}, {$rename:{aa:'c'},$set:{b:5}}, {b:5,c:1} );
good( {a:1,b:2}, {$rename:{z:'c'},$set:{b:5}}, {a:1,b:5} );
good( {aa:1,b:2}, {$rename:{z:'c'},$set:{b:5}}, {aa:1,b:5} );

// (formerly) rewriting single field
good( {a:{z:1,b:1}}, {$rename:{'a.b':'a.c'}}, {a:{c:1,z:1}} );
good( {a:{z:1,tomato:1}}, {$rename:{'a.tomato':'a.potato'}}, {a:{potato:1,z:1}} );
good( {a:{z:1,b:1,c:1}}, {$rename:{'a.b':'a.c'}}, {a:{c:1,z:1}} );
good( {a:{z:1,tomato:1,potato:1}}, {$rename:{'a.tomato':'a.potato'}}, {a:{potato:1,z:1}} );
good( {a:{z:1,b:1}}, {$rename:{'a.b':'a.cc'}}, {a:{cc:1,z:1}} );
good( {a:{z:1,b:1,c:1}}, {$rename:{'a.b':'aa.c'}}, {a:{c:1,z:1},aa:{c:1}} );

// invalid target, but missing source
good( {a:1,c:4}, {$rename:{b:'c.d'}}, {a:1,c:4} );

// TODO: This should be supported, and it is with the new update framework, but not with the
// old, and we currently don't have a good way to check which mode we are in. When we do have
// that, add this test guarded under that condition. Or, when we remove the old update path
// just enable this test.

// valid to rename away from an invalid name
// good( {x:1}, {$rename:{'$a.b':'a.b'}}, {x:1} );

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
    assert.docEq( expected, got );
}

l( {a:1}, {$rename:{a:'b'}}, {a:null}, {b:1} );
l( {a:1}, {$rename:{a:'bb'}}, {a:null}, {bb:1} );
l( {b:1}, {$rename:{b:'a'}}, {a:1}, {a:1} );
l( {bb:1}, {$rename:{bb:'a'}}, {a:1}, {a:1} );
