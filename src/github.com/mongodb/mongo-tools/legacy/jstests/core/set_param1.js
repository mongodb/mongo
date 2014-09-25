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
// component verbosity
//

// verbosity for log component hierarchy
printjson(old.logComponentVerbosity);
assert.neq( undefined, old.logComponentVerbosity, "log component verbosity not available" );
assert.eq( old.logLevel, old.logComponentVerbosity.verbosity,
           "default component verbosity should match logLevel" );
assert.neq( undefined, old.logComponentVerbosity.storage.journaling.verbosity,
            "journaling verbosity not available" );

// Non-object log component verbosity should be rejected.
assert.commandFailed(db.adminCommand( { "setParameter" : 1 ,
                                        logComponentVerbosity : "not an object" } ) );

// Non-numeric verbosity for component should be rejected.
assert.commandFailed( db.adminCommand( { "setParameter" : 1 ,
                                          logComponentVerbosity :
                                              { storage : { journaling : {
                                                  verbosity : "not a number" } } } } ) );

// Set multiple component log levels at once.
// Unrecognized field names not mapping to a log component will be ignored.
var res1 = assert.commandWorked( db.adminCommand( {
    "setParameter" : 1 ,
    logComponentVerbosity : {
        verbosity : 2,
        accessControl : { verbosity : 0 },
        storage : {
            verbosity : 3,
            journaling : { verbosity : 5 }
        },
        NoSuchComponent : { verbosity : 2 },
        extraField : 123 } } ) );

// Clear verbosity for default and journaling.
var res2 = assert.commandWorked( db.adminCommand( {
    "setParameter" : 1 ,
    logComponentVerbosity : {
        verbosity: -1,
        storage : {
            journaling : { verbosity : -1 } } } } ) );
assert.eq( 2, res2.was.verbosity );
assert.eq( 3, res2.was.storage.verbosity );
assert.eq( 5, res2.was.storage.journaling.verbosity );

// Set accessControl verbosity using numerical level instead of
// subdocument with 'verbosity' field.
var res3 = assert.commandWorked( db.adminCommand( {
    "setParameter" : 1,
    logComponentVerbosity : {
        accessControl : 5  } } ) );
assert.eq( 0, res3.was.accessControl.verbosity );

var res4 = assert.commandWorked( db.adminCommand( {
    "getParameter" : 1,
    logComponentVerbosity : 1 } ) );
assert.eq( 5, res4.logComponentVerbosity.accessControl.verbosity );

// Restore old verbosity values.
assert.commandWorked( db.adminCommand( {
    "setParameter" : 1 ,
    logComponentVerbosity : old.logComponentVerbosity } ) );
