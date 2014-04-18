// test for SERVER-5013
// make sure very long long lines get truncated

t = db.loglong;
t.drop();

t.insert( { x : 1 } );

n = 0;
query = { x : [] }
while ( Object.bsonsize( query ) < 30000 ) {
    query.x.push( n++ );
}

before = db.adminCommand( { setParameter : 1 , logLevel : 1 } )

t.findOne( query )

x = db.adminCommand( { setParameter : 1 , logLevel : before.was } )
assert.eq( 1 , x.was , tojson( x ) )

log = db.adminCommand( { getLog : "global" } ).log

found = false
for ( i=log.length - 1; i>= 0; i-- ) {
    if ( log[i].indexOf( "warning: log line attempted (16k)" ) >= 0 ) {
        found = true;
        break;
    }
}

assert( found )
