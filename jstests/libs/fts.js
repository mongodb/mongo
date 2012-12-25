
// make sure we're enabled
db.adminCommand( { setParameter : "*", textSearchEnabled : true } );

function queryIDS( coll, search, filter, extra ){
    var cmd = { search : search }
    if ( filter )
        cmd.filter = filter;
    if ( extra )
        Object.extend( cmd, extra );
    lastCommadResult = coll.runCommand( "text" , cmd);

    return getIDS( lastCommadResult );
}

function getIDS( commandResult ){
    if ( ! ( commandResult && commandResult.results ) )
        return []

    return commandResult.results.map( function(z){ return z.obj._id; } )
}
