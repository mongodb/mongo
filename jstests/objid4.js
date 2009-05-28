


o = new ObjectId();
assert( o.str );

a = new ObjectId( o.str );
assert.eq( o.str , a.str );
assert.eq( a.str , a.str.toString() )

b = ObjectId( o.str );
assert.eq( o.str , b.str );
assert.eq( b.str , b.str.toString() )

