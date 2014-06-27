// Tests for accessing logLevel server parameter using getParameter/setParameter commands
// and shell helpers.

old = db.adminCommand( { "getParameter" : "*" } )
tmp1 = db.adminCommand( { "setParameter" : 1 , "logLevel" : 5 } )
tmp2 = db.adminCommand( { "setParameter" : 1 , "logLevel" : old.logLevel } )
now = db.adminCommand( { "getParameter" : "*" } )

assert.eq( old , now , "A" )
assert.eq( old.logLevel , tmp1.was , "B" )
assert.eq( 5 , tmp2.was , "C" )

//
// tag log levels
//

// log level server parameters for tags start with logLevel_
var logLevelParameterFound = false;
var logLevelParameterPrefix = "logLevel_";
var logTags = [];
for ( var key in old ) {
    if ( key.indexOf( logLevelParameterPrefix ) != 0) {
        continue;
    }
    logLevelParameterFound = true;

    // Get tag name.
    var tag = key.substring( logLevelParameterPrefix.length );
    logTags.push( tag );

    // Save current log level.
    var logLevelParameter = key;
    var getArgs = { "getParameter" : 1 };
    getArgs[ logLevelParameter ] = 1;
    var current = db.adminCommand( getArgs );
    assert.commandWorked( current, logLevelParameter );
    var currentLogLevel = current[ logLevelParameter ];

    // Set debug level to 5.
    var setArgs = { "setParameter" : 1 };
    setArgs[ logLevelParameter ] = 5;
    assert.commandWorked( db.adminCommand( setArgs ), logLevelParameter );

    // Get debug level after setParameter.
    assert.eq( 5, db.adminCommand( getArgs )[ logLevelParameter ] );

    // Negative log level means to clear tag's log level.
    setArgs[ logLevelParameter ] = -1;
    assert.commandWorked( db.adminCommand( setArgs ), logLevelParameter );

    // Non-numeric log levels should be rejected.
    setArgs[logLevelParameter] = "5";
    assert.commandFailed( db.adminCommand( setArgs ), logLevelParameter );

    // Restore old log level.
    setArgs[logLevelParameter] = currentLogLevel;
    assert.commandWorked( db.adminCommand( setArgs ), logLevelParameter );
}
assert( logLevelParameterFound, "no log level parameters for tags found" );
