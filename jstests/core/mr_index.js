
t = db.mr_index
t.drop()

outName = "mr_index_out"
out = db[outName]
out.drop()

t.insert( { tags : [ 1  ] } )
t.insert( { tags : [ 1 , 2  ] } )
t.insert( { tags : [ 1 , 2 , 3 ] } )
t.insert( { tags : [ 3 ] } )
t.insert( { tags : [ 2 , 3 ] } )
t.insert( { tags : [ 2 , 3 ] } )
t.insert( { tags : [ 1 , 2 ] } )

m = function(){ 
    for ( i=0; i<this.tags.length; i++ )
        emit( this.tags[i] , 1 );
}

r = function( k , vs ){
    return Array.sum( vs );
}

ex = function(){
    return out.find().sort( { value : 1 } ).explain()
}

res = t.mapReduce(  m , r , { out : outName } )
    
assert.eq( "BasicCursor" , ex().cursor , "A1" )
out.ensureIndex( { value : 1 } )
assert.eq( "BtreeCursor value_1" , ex().cursor , "A2" )
assert.eq( 3 , ex().n , "A3" )

res = t.mapReduce(  m , r , { out : outName } )
    
assert.eq( "BtreeCursor value_1" , ex().cursor , "B1" )
assert.eq( 3 , ex().n , "B2" )
res.drop()


