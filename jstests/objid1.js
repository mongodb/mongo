t = db.objid1;
t.drop();

b = new ObjectId();
assert( b.str , "objid1 test1" );

a = ObjectId( b.str );
assert.eq( a.str , b.str );

t.save( { a : a } )
assert( t.findOne().a.isObjectId );
assert.eq( a.str , t.findOne().a.str );

