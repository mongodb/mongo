
x = { quotes:"a\"b" , nulls:null };
eval( "y = " + tojson( x ) );
assert.eq( tojson( x ) , tojson( y ) );
assert.eq( typeof( x.nulls ) , typeof( y.nulls ) );
