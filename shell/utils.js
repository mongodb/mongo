
assert = function( b , msg ){
    if ( b )
        return;
    
    throw "assert failed : " + msg;
}

assert.eq = function( a , b , msg ){
    if ( a == b )
        return;

    throw "[" + a + "] != [" + b + "] are not equal : " + msg;
}

assert.soon = function( f ) {
    var start = new Date();
    var last;
    while( 1 ) {
        if ( f() )
            return;
        if ( ( new Date() ).getTime() - start.getTime() > 10000 )
            throw "assert.soon failed: " + f;
        sleep( 200 );
    }
}

Object.extend = function( dst , src ){
    for ( var k in src ){
        dst[k] = src[k];
    }
    return dst;
}

argumentsToArray = function( a ){
    var arr = [];
    for ( var i=0; i<a.length; i++ )
        arr[i] = a[i];
    return arr;
}

isString = function( x ){
    return typeof( x ) == "string";
}

isObject = function( x ){
    return typeof( x ) == "object";
}

String.prototype.trim = function() {
    return this.replace(/^\s+|\s+$/g,"");
}
String.prototype.ltrim = function() {
    return this.replace(/^\s+/,"");
}
String.prototype.rtrim = function() {
    return this.replace(/\s+$/,"");
}

Date.timeFunc = function( theFunc , numTimes ){

    var start = new Date();
    
    numTimes = numTimes || 1;
    for ( var i=0; i<numTimes; i++ ){
        theFunc.apply( null , argumentsToArray( arguments ).slice( 2 ) );
    }

    return (new Date()).getTime() - start.getTime();
}

Date.prototype.tojson = function(){
    return "\"" + this.toString() + "\"";
}

RegExp.prototype.tojson = RegExp.prototype.toString;

Array.prototype.tojson = function(){
    var s = "[";
    for ( var i=0; i<this.length; i++){
        if ( i > 0 )
            s += ",";
        s += tojson( this[i] );
    }
    s += "]";
    return s;
}

ObjectId.prototype.toString = function(){
    return this.str;
}

ObjectId.prototype.tojson = function(){
    return "\"" + this.str + "\"";
}

ObjectId.prototype.isObjectId = true;

tojson = function( x ){
    if ( x == null || x == undefined )
        return "";
    
    switch ( typeof x ){
        
    case "string": 
        return "\"" + x + "\"";
        
    case "number": 
    case "boolean":
        return "" + x;
            
    case "object":
        return tojsonObject( x );
        
    default:
        throw "can't handle type " + ( typeof v );
    }
    
}

tojsonObject = function( x ){
    assert( typeof x == "object" , "tojsonObject needs object" );
    
    if ( x.tojson )
        return x.tojson();

    var s = "{";
    
    var first = true;
    for ( var k in x ){
        if ( first ) first = false;
        else s += " , ";
        
        s += "\"" + k + "\" : " + tojson( x[k] );
    }

    return s + "}";
}

shellPrint = function( x ){
    if ( x != undefined )
        shellPrintHelper( x );
    
    if ( db ){
        var e = db.getPrevError();
        if ( e.err ) {
	    if( e.nPrev <= 1 )
		print( "error on last call: " + tojson( e.err ) );
	    else
		print( "an error " + tojson(e.err) + " occurred " + e.nPrev + " operations back in the command invocation" );
        }
        db.resetError();
    }
}

shellPrintHelper = function( x ){
    
    if ( typeof x != "object" ) 
        return print( x );
    
    var p = x.shellPrint;
    if ( typeof p == "function" )
        return x.shellPrint();

    var p = x.tojson;
    if ( typeof p == "function" )
        print( x.tojson() );
    else
        print( tojson( x ) );
}

shellHelper = function( command , rest ){
    command = command.trim();
    var args = rest.trim().replace(/;$/,"").split( "\s+" );
    
    if ( ! shellHelper[command] )
        throw "no command [" + command + "]";
    
    return shellHelper[command].apply( null , args );
}

help = shellHelper.help = function(){
    print( "HELP" );
    print( "\t" + "show (dbs|collections|users)" );
    print( "\t" + "use <db name>" );
    print( "\t" + "db.help()                    help on DB methods");
    print( "\t" + "db.foo.find()" );
    print( "\t" + "db.foo.find( { a : 1 } )" );
    print( "\t" + "db.foo.help()                help on collection methods");
}

shellHelper.use = function( dbname ){
    db = db.getMongo().getDB( dbname );
    print( "switched to db " + db.getName() );
}

shellHelper.show = function( what ){
    assert( typeof what == "string" );
    
    if ( what == "users" )
	return db.system.users.find();

    if ( what == "collections" || what == "tables" )
        return db.getCollectionNames();
    
    if ( what == "dbs" )
        return db.getMongo().getDBNames();
    
    throw "don't know how to show [" + what + "]";

}
