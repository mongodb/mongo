NUM = 20;
M = 5;

t = db.jstests_arr2;

function test( withIndex ){
    t.drop();
    
    // insert a bunch of items to force queries to use the index.
    newObject = {
        _id : 1,
        a : [
            { b : { c : 1 }  }
        ]
    }
    
    now = (new Date()).getTime() / 1000;
    for (created = now - NUM; created <= now; created++ ) {
        newObject['created'] = created;
        t.insert(newObject);
        newObject['_id'] ++;
    }
    
    // change the last M items.
    query = {
        'created' : { '$gte' : now - M }
    }
    
    Z = t.find( query ).count();
    
    if ( withIndex ){
        //t.ensureIndex( { 'a.b.c' : 1, 'created' : -1 } )
        //t.ensureIndex( { created : -1 } )
        t.ensureIndex( { 'a.b.c' : 1 } , { name : "x" } )
    }
    
    t.update(query, { '$set' : { "a.0.b.c" : 0 } } , false , true )
    assert.eq( Z , db.getLastErrorObj().n , "num updated withIndex:" + withIndex );
    
    // now see how many were actually updated.
    query['a.b.c'] = 0;
    
    count = t.count(query);

    assert.eq( Z , count , "count after withIndex:" + withIndex );
}

test( false )
test( true );


