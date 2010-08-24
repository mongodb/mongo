__quiet = false;
__magicNoPrint = { __magicNoPrint : 1111 }

chatty = function(s){
    if ( ! __quiet )
        print( s );
}

friendlyEqual = function( a , b ){
    if ( a == b )
        return true;

    if ( tojson( a ) == tojson( b ) )
        return true;

    return false;
}

printStackTrace = function(){
    try{
        throw new Error("Printing Stack Trace (lines are 0-based in spidermonkey)");
    } catch (e) {
        print(e.stack);
    }
}

doassert = function (msg) {
    if (msg.indexOf("assert") == 0)
        print(msg);
    else
        print("assert: " + msg);
    printStackTrace();
    throw msg;
}

assert = function( b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( b )
        return;
    
    doassert( msg == undefined ? "assert failed" : "assert failed : " + msg );
}

assert.automsg = function( b ) {
    assert( eval( b ), b );
}

assert._debug = false;

assert.eq = function( a , b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( a == b )
        return;

    if ( ( a != null && b != null ) && friendlyEqual( a , b ) )
        return;

    doassert( "[" + tojson( a ) + "] != [" + tojson( b ) + "] are not equal : " + msg );
}

assert.eq.automsg = function( a, b ) {
    assert.eq( eval( a ), eval( b ), "[" + a + "] != [" + b + "]" );
}

assert.neq = function( a , b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );
    if ( a != b )
        return;

    doassert( "[" + a + "] != [" + b + "] are equal : " + msg );
}

assert.repeat = function( f, msg, timeout, interval ) {
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    var start = new Date();
    timeout = timeout || 30000;
    interval = interval || 200;
    var last;
    while( 1 ) {
        
        if ( typeof( f ) == "string" ){
            if ( eval( f ) )
                return;
        }
        else {
            if ( f() )
                return;
        }
        
        if ( ( new Date() ).getTime() - start.getTime() > timeout )
            break;
        sleep( interval );
    }
}
    
assert.soon = function( f, msg, timeout, interval ) {
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    var start = new Date();
    timeout = timeout || 30000;
    interval = interval || 200;
    var last;
    while( 1 ) {
        
        if ( typeof( f ) == "string" ){
            if ( eval( f ) )
                return;
        }
        else {
            if ( f() )
                return;
        }
        
        if ( ( new Date() ).getTime() - start.getTime() > timeout )
            doassert( "assert.soon failed: " + f + ", msg:" + msg );
        sleep( interval );
    }
}

assert.throws = function( func , params , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );
    try {
        func.apply( null , params );
    }
    catch ( e ){
        return e;
    }

    doassert( "did not throw exception: " + msg );
}

assert.throws.automsg = function( func, params ) {
    assert.throws( func, params, func.toString() );
}

assert.commandWorked = function( res , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( res.ok == 1 )
        return;
    
    doassert( "command failed: " + tojson( res ) + " : " + msg );
}

assert.commandFailed = function( res , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( res.ok == 0 )
        return;
    
    doassert( "command worked when it should have failed: " + tojson( res ) + " : " + msg );
}

assert.isnull = function( what , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( what == null )
        return;
    
    doassert( "supposed to null (" + ( msg || "" ) + ") was: " + tojson( what ) );
}

assert.lt = function( a , b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( a < b )
        return;
    doassert( a + " is not less than " + b + " : " + msg );
}

assert.gt = function( a , b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( a > b )
        return;
    doassert( a + " is not greater than " + b + " : " + msg );
}

assert.lte = function( a , b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( a <= b )
        return;
    doassert( a + " is not less than or eq " + b + " : " + msg );
}

assert.gte = function( a , b , msg ){
    if ( assert._debug && msg ) print( "in assert for: " + msg );

    if ( a >= b )
        return;
    doassert( a + " is not greater than or eq " + b + " : " + msg );
}


assert.close = function( a , b , msg , places ){
    if (places === undefined) {
        places = 4;
    }
    if (Math.round((a - b) * Math.pow(10, places)) === 0) {
        return;
    }
    doassert( a + " is not equal to " + b + " within " + places +
              " places, diff: " + (a-b) + " : " + msg );
};

Object.extend = function( dst , src , deep ){
    for ( var k in src ){
        var v = src[k];
        if ( deep && typeof(v) == "object" ){
            v = Object.extend( typeof ( v.length ) == "number" ? [] : {} , v , true );
        }
        dst[k] = v;
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

isNumber = function(x){
    return typeof( x ) == "number";
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

Number.prototype.zeroPad = function(width) {
    var str = this + '';
    while (str.length < width)
        str = '0' + str;
    return str;
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

    var UTC = Date.printAsUTC ? 'UTC' : '';

    var year = this['get'+UTC+'FullYear']().zeroPad(4);
    var month = (this['get'+UTC+'Month']() + 1).zeroPad(2);
    var date = this['get'+UTC+'Date']().zeroPad(2);
    var hour = this['get'+UTC+'Hours']().zeroPad(2);
    var minute = this['get'+UTC+'Minutes']().zeroPad(2);
    var sec = this['get'+UTC+'Seconds']().zeroPad(2)

    if (this['get'+UTC+'Milliseconds']())
        sec += '.' + this['get'+UTC+'Milliseconds']().zeroPad(3)

    var ofs = 'Z';
    if (!Date.printAsUTC){
        var ofsmin = this.getTimezoneOffset();
        if (ofsmin != 0){
            ofs = ofsmin > 0 ? '-' : '+'; // This is correct
            ofs += (ofsmin/60).zeroPad(2)
            ofs += (ofsmin%60).zeroPad(2)
        }
    }

    return 'ISODate("'+year+'-'+month+'-'+date+'T'+hour+':'+minute+':'+sec+ofs+'")';
}

Date.printAsUTC = true;


ISODate = function(isoDateStr){
    if (!isoDateStr)
        return new Date();

    var isoDateRegex = /(\d{4})-?(\d{2})-?(\d{2})([T ](\d{2})(:?(\d{2})(:?(\d{2}(\.\d+)?))?)?(Z|([+-])(\d{2}):?(\d{2})?)?)?/;
    var res = isoDateRegex.exec(isoDateStr);

    if (!res)
        throw "invalid ISO date";

    var year = parseInt(res[1],10) || 1970; // this should always be present
    var month = (parseInt(res[2],10) || 1) - 1;
    var date = parseInt(res[3],10) || 0;
    var hour = parseInt(res[5],10) || 0;
    var min = parseInt(res[7],10) || 0;
    var sec = parseFloat(res[9]) || 0;
    var ms = Math.round((sec%1) * 1000)
    sec -= ms/1000

    var time = Date.UTC(year, month, date, hour, min, sec, ms);

    if (res[11] && res[11] != 'Z'){
        var ofs = 0;
        ofs += (parseInt(res[13],10) || 0) * 60*60*1000; // hours
        ofs += (parseInt(res[14],10) || 0) *    60*1000; // mins
        if (res[12] == '+') // if ahead subtract
            ofs *= -1;

        time += ofs
    }

    return new Date(time);
}

RegExp.prototype.tojson = RegExp.prototype.toString;

Array.contains = function( a  , x ){
    for ( var i=0; i<a.length; i++ ){
        if ( a[i] == x )
            return true;
    }
    return false;
}

Array.unique = function( a ){
    var u = [];
    for ( var i=0; i<a.length; i++){
        var o = a[i];
        if ( ! Array.contains( u , o ) ){
            u.push( o );
        }
    }
    return u;
}

Array.shuffle = function( arr ){
    for ( var i=0; i<arr.length-1; i++ ){
        var pos = i+Random.randInt(arr.length-i);
        var save = arr[i];
        arr[i] = arr[pos];
        arr[pos] = save;
    }
    return arr;
}


Array.tojson = function( a , indent ){
    if (!indent) 
        indent = "";

    if (a.length == 0) {
        return "[ ]";
    }

    var s = "[\n";
    indent += "\t";
    for ( var i=0; i<a.length; i++){
        s += indent + tojson( a[i], indent );
        if ( i < a.length - 1 ){
            s += ",\n";
        }
    }
    if ( a.length == 0 ) {
        s += indent;
    }

    indent = indent.substring(1);
    s += "\n"+indent+"]";
    return s;
}

Array.fetchRefs = function( arr , coll ){
    var n = [];
    for ( var i=0; i<arr.length; i ++){
        var z = arr[i];
        if ( coll && coll != z.getCollection() )
            continue;
        n.push( z.fetch() );
    }
    
    return n;
}

Array.sum = function( arr ){
    if ( arr.length == 0 )
        return null;
    var s = arr[0];
    for ( var i=1; i<arr.length; i++ )
        s += arr[i];
    return s;
}

Array.avg = function( arr ){
    if ( arr.length == 0 )
        return null;
    return Array.sum( arr ) / arr.length;
}

Array.stdDev = function( arr ){
    var avg = Array.avg( arr );
    var sum = 0;

    for ( var i=0; i<arr.length; i++ ){
        sum += Math.pow( arr[i] - avg , 2 );
    }

    return Math.sqrt( sum / arr.length );
}

Object.keySet = function( o ) {
    var ret = new Array();
    for( i in o ) {
        if ( !( i in o.__proto__ && o[ i ] === o.__proto__[ i ] ) ) {
            ret.push( i );
        }
    }
    return ret;
}

if ( ! NumberLong.prototype ) {
    NumberLong.prototype = {}
}

NumberLong.prototype.tojson = function() {
    return this.toString();
}

if ( ! ObjectId.prototype )
    ObjectId.prototype = {}

ObjectId.prototype.toString = function(){
    return this.str;
}

ObjectId.prototype.tojson = function(){
    return "ObjectId(\"" + this.str + "\")";
}

ObjectId.prototype.isObjectId = true;

ObjectId.prototype.getTimestamp = function(){
    return new Date(parseInt(this.toString().slice(0,8), 16)*1000);
}

ObjectId.prototype.equals = function( other){
    return this.str == other.str;
}

if ( typeof( DBPointer ) != "undefined" ){
    DBPointer.prototype.fetch = function(){
        assert( this.ns , "need a ns" );
        assert( this.id , "need an id" );
        
        return db[ this.ns ].findOne( { _id : this.id } );
    }
    
    DBPointer.prototype.tojson = function(indent){
        return tojson({"ns" : this.ns, "id" : this.id}, indent);
    }

    DBPointer.prototype.getCollection = function(){
        return this.ns;
    }
    
    DBPointer.prototype.toString = function(){
        return "DBPointer " + this.ns + ":" + this.id;
    }
}
else {
    print( "warning: no DBPointer" );
}

if ( typeof( DBRef ) != "undefined" ){
    DBRef.prototype.fetch = function(){
        assert( this.$ref , "need a ns" );
        assert( this.$id , "need an id" );
        
        return db[ this.$ref ].findOne( { _id : this.$id } );
    }
    
    DBRef.prototype.tojson = function(indent){
        return tojson({"$ref" : this.$ref, "$id" : this.$id}, indent);
    }

    DBRef.prototype.getCollection = function(){
        return this.$ref;
    }
    
    DBRef.prototype.toString = function(){
        return this.tojson();
    }
}
else {
    print( "warning: no DBRef" );
}

if ( typeof( BinData ) != "undefined" ){
    BinData.prototype.tojson = function () {
        //return "BinData type: " + this.type + " len: " + this.len;
        return this.toString();
    }
}
else {
    print( "warning: no BinData class" );
}

if ( typeof( UUID ) != "undefined" ){
    UUID.prototype.tojson = function () {
        return this.toString();
    }
}

if ( typeof _threadInject != "undefined" ){
    print( "fork() available!" );
    
    Thread = function(){
        this.init.apply( this, arguments );
    }
    _threadInject( Thread.prototype );
    
    ScopedThread = function() {
        this.init.apply( this, arguments );
    }
    ScopedThread.prototype = new Thread( function() {} );
    _scopedThreadInject( ScopedThread.prototype );
    
    fork = function() {
        var t = new Thread( function() {} );
        Thread.apply( t, arguments );
        return t;
    }    

    // Helper class to generate a list of events which may be executed by a ParallelTester
    EventGenerator = function( me, collectionName, mean ) {
        this.mean = mean;
        this.events = new Array( me, collectionName );
    }
    
    EventGenerator.prototype._add = function( action ) {
        this.events.push( [ Random.genExp( this.mean ), action ] );
    }
    
    EventGenerator.prototype.addInsert = function( obj ) {
        this._add( "t.insert( " + tojson( obj ) + " )" );
    }

    EventGenerator.prototype.addRemove = function( obj ) {
        this._add( "t.remove( " + tojson( obj ) + " )" );
    }

    EventGenerator.prototype.addUpdate = function( objOld, objNew ) {
        this._add( "t.update( " + tojson( objOld ) + ", " + tojson( objNew ) + " )" );
    }
    
    EventGenerator.prototype.addCheckCount = function( count, query, shouldPrint, checkQuery ) {
        query = query || {};
        shouldPrint = shouldPrint || false;
        checkQuery = checkQuery || false;
        var action = "assert.eq( " + count + ", t.count( " + tojson( query ) + " ) );"
        if ( checkQuery ) {
            action += " assert.eq( " + count + ", t.find( " + tojson( query ) + " ).toArray().length );"
        }
        if ( shouldPrint ) {
            action += " print( me + ' ' + " + count + " );";
        }
        this._add( action );
    }
    
    EventGenerator.prototype.getEvents = function() {
        return this.events;
    }
    
    EventGenerator.dispatch = function() {
        var args = argumentsToArray( arguments );
        var me = args.shift();
        var collectionName = args.shift();
        var m = new Mongo( db.getMongo().host );
        var t = m.getDB( "test" )[ collectionName ];
        for( var i in args ) {
            sleep( args[ i ][ 0 ] );
            eval( args[ i ][ 1 ] );
        }
    }
    
    // Helper class for running tests in parallel.  It assembles a set of tests
    // and then calls assert.parallelests to run them.
    ParallelTester = function() {
        this.params = new Array();
    }
    
    ParallelTester.prototype.add = function( fun, args ) {
        args = args || [];
        args.unshift( fun );
        this.params.push( args );
    }
    
    ParallelTester.prototype.run = function( msg, newScopes ) {
        newScopes = newScopes || false;
        assert.parallelTests( this.params, msg, newScopes );
    }
    
    // creates lists of tests from jstests dir in a format suitable for use by
    // ParallelTester.fileTester.  The lists will be in random order.
    // n: number of lists to split these tests into
    ParallelTester.createJstestsLists = function( n ) {
        var params = new Array();
        for( var i = 0; i < n; ++i ) {
            params.push( [] );
        }

        var makeKeys = function( a ) {
            var ret = {};
            for( var i in a ) {
                ret[ a[ i ] ] = 1;
            }
            return ret;
        }
        
        // some tests can't run in parallel with most others
        var skipTests = makeKeys( [ "jstests/dbadmin.js",
                                   "jstests/repair.js",
                                   "jstests/cursor8.js",
                                   "jstests/recstore.js",
                                   "jstests/extent.js",
                                   "jstests/indexb.js",
                                   "jstests/profile1.js",
                                   "jstests/mr3.js",
                                   "jstests/indexh.js",
                                   "jstests/apitest_db.js",
                                   "jstests/evalb.js"] );
        
        // some tests can't be run in parallel with each other
        var serialTestsArr = [ "jstests/fsync.js",
                              "jstests/fsync2.js" ];
        var serialTests = makeKeys( serialTestsArr );
        
        params[ 0 ] = serialTestsArr;
        
        var files = listFiles("jstests");
        files = Array.shuffle( files );
        
        var i = 0;
        files.forEach(
                      function(x) {
                      
                      if ( ( /[\/\\]_/.test(x.name) ) ||
                          ( ! /\.js$/.test(x.name ) ) ||
                          ( x.name in skipTests ) ||
                          ( x.name in serialTests ) ||
                          ! /\.js$/.test(x.name ) ){ 
                      print(" >>>>>>>>>>>>>>> skipping " + x.name);
                      return;
                      }
                      
                      params[ i % n ].push( x.name );
                      ++i;
                      }
        );
        
        // randomize ordering of the serialTests
        params[ 0 ] = Array.shuffle( params[ 0 ] );
        
        for( var i in params ) {
            params[ i ].unshift( i );
        }
        
        return params;
    }
    
    // runs a set of test files
    // first argument is an identifier for this tester, remaining arguments are file names
    ParallelTester.fileTester = function() {
        var args = argumentsToArray( arguments );
        var suite = args.shift();
        args.forEach(
                     function( x ) {
                     print("         S" + suite + " Test : " + x + " ...");
                     var time = Date.timeFunc( function() { load(x); }, 1);
                     print("         S" + suite + " Test : " + x + " " + time + "ms" );
                     }
                     );        
    }
    
    // params: array of arrays, each element of which consists of a function followed
    // by zero or more arguments to that function.  Each function and its arguments will
    // be called in a separate thread.
    // msg: failure message
    // newScopes: if true, each thread starts in a fresh scope
    assert.parallelTests = function( params, msg, newScopes ) {
        newScopes = newScopes || false;
        var wrapper = function( fun, argv ) {
                   eval (
                         "var z = function() {" +
                         "var __parallelTests__fun = " + fun.toString() + ";" +
                         "var __parallelTests__argv = " + tojson( argv ) + ";" +
                         "var __parallelTests__passed = false;" +
                         "try {" +
                            "__parallelTests__fun.apply( 0, __parallelTests__argv );" +
                            "__parallelTests__passed = true;" +
                         "} catch ( e ) {" +
                            "print( e );" +
                         "}" +
                         "return __parallelTests__passed;" +
                         "}"
                         );
            return z;
        }
        var runners = new Array();
        for( var i in params ) {
            var param = params[ i ];
            var test = param.shift();
            var t;
            if ( newScopes )
                t = new ScopedThread( wrapper( test, param ) );
            else
                t = new Thread( wrapper( test, param ) );
            runners.push( t );
        }
        
        runners.forEach( function( x ) { x.start(); } );
        var nFailed = 0;
        // v8 doesn't like it if we exit before all threads are joined (SERVER-529)
        runners.forEach( function( x ) { if( !x.returnData() ) { ++nFailed; } } );        
        assert.eq( 0, nFailed, msg );
    }
}

tojsononeline = function( x ){
    return tojson( x , " " , true );
}

tojson = function( x, indent , nolint ){
    if ( x === null )
        return "null";
    
    if ( x === undefined )
        return "undefined";
    
    if (!indent) 
        indent = "";

    switch ( typeof x ) {
    case "string": {
        var s = "\"";
        for ( var i=0; i<x.length; i++ ){
            switch (x[i]){
                case '"': s += '\\"'; break;
                case '\\': s += '\\\\'; break;
                case '\b': s += '\\b'; break;
                case '\f': s += '\\f'; break;
                case '\n': s += '\\n'; break;
                case '\r': s += '\\r'; break;
                case '\t': s += '\\t'; break;

                default: {
                    var code = x.charCodeAt(i);
                    if (code < 0x20){
                        s += (code < 0x10 ? '\\u000' : '\\u00') + code.toString(16);
                    } else {
                        s += x[i];
                    }
                }
            }
        }
        return s + "\"";
    }
    case "number": 
    case "boolean":
        return "" + x;
    case "object":{
        var s = tojsonObject( x, indent , nolint );
        if ( ( nolint == null || nolint == true ) && s.length < 80 && ( indent == null || indent.length == 0 ) ){
            s = s.replace( /[\s\r\n ]+/gm , " " );
        }
        return s;
    }
    case "function":
        return x.toString();
    default:
        throw "tojson can't handle type " + ( typeof x );
    }
    
}

tojsonObject = function( x, indent , nolint ){
    var lineEnding = nolint ? " " : "\n";
    var tabSpace = nolint ? "" : "\t";
    
    assert.eq( ( typeof x ) , "object" , "tojsonObject needs object, not [" + ( typeof x ) + "]" );

    if (!indent) 
        indent = "";
    
    if ( typeof( x.tojson ) == "function" && x.tojson != tojson ) {
        return x.tojson(indent,nolint);
    }
    
    if ( x.constructor && typeof( x.constructor.tojson ) == "function" && x.constructor.tojson != tojson ) {
        return x.constructor.tojson( x, indent , nolint );
    }

    if ( x.toString() == "[object MaxKey]" )
        return "{ $maxKey : 1 }";
    if ( x.toString() == "[object MinKey]" )
        return "{ $minKey : 1 }";
    
    var s = "{" + lineEnding;

    // push one level of indent
    indent += tabSpace;
    
    var total = 0;
    for ( var k in x ) total++;
    if ( total == 0 ) {
        s += indent + lineEnding;
    }

    var keys = x;
    if ( typeof( x._simpleKeys ) == "function" )
        keys = x._simpleKeys();
    var num = 1;
    for ( var k in keys ){
        
        var val = x[k];
        if ( val == DB.prototype || val == DBCollection.prototype )
            continue;

        s += indent + "\"" + k + "\" : " + tojson( val, indent , nolint );
        if (num != total) {
            s += ",";
            num++;
        }
        s += lineEnding;
    }

    // pop one level of indent
    indent = indent.substring(1);
    return s + indent + "}";
}

shellPrint = function( x ){
    it = x;
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

printjson = function(x){
    print( tojson( x ) );
}

printjsononeline = function(x){
    print( tojsononeline( x ) );
}

shellPrintHelper = function (x) {

    if (typeof (x) == "undefined") {

        if (typeof (db) != "undefined" && db.getLastError) {
            // explicit w:1 so that replset getLastErrorDefaults aren't used here which would be bad.
            var e = db.getLastError(1);
            if (e != null)
                print(e);
        }

        return;
    }

    if (x == __magicNoPrint)
        return;

    if (x == null) {
        print("null");
        return;
    }

    if (typeof x != "object")
        return print(x);

    var p = x.shellPrint;
    if (typeof p == "function")
        return x.shellPrint();

    var p = x.tojson;
    if (typeof p == "function")
        print(x.tojson());
    else
        print(tojson(x));
}

shellAutocomplete = function (/*prefix*/){ // outer scope function called on init. Actual function at end

    var universalMethods = "constructor prototype toString valueOf toLocaleString hasOwnProperty propertyIsEnumerable".split(' ');

    var builtinMethods = {}; // uses constructor objects as keys
    builtinMethods[Array] = "length concat join pop push reverse shift slice sort splice unshift indexOf lastIndexOf every filter forEach map some".split(' ');
    builtinMethods[Boolean] = "".split(' '); // nothing more than universal methods
    builtinMethods[Date] = "getDate getDay getFullYear getHours getMilliseconds getMinutes getMonth getSeconds getTime getTimezoneOffset getUTCDate getUTCDay getUTCFullYear getUTCHours getUTCMilliseconds getUTCMinutes getUTCMonth getUTCSeconds getYear parse setDate setFullYear setHours setMilliseconds setMinutes setMonth setSeconds setTime setUTCDate setUTCFullYear setUTCHours setUTCMilliseconds setUTCMinutes setUTCMonth setUTCSeconds setYear toDateString toGMTString toLocaleDateString toLocaleTimeString toTimeString toUTCString UTC".split(' ');
    builtinMethods[Math] = "E LN2 LN10 LOG2E LOG10E PI SQRT1_2 SQRT2 abs acos asin atan atan2 ceil cos exp floor log max min pow random round sin sqrt tan".split(' ');
    builtinMethods[Number] = "MAX_VALUE MIN_VALUE NEGATIVE_INFINITY POSITIVE_INFINITY toExponential toFixed toPrecision".split(' ');
    builtinMethods[RegExp] = "global ignoreCase lastIndex multiline source compile exec test".split(' ');
    builtinMethods[String] = "length charAt charCodeAt concat fromCharCode indexOf lastIndexOf match replace search slice split substr substring toLowerCase toUpperCase".split(' ');
    builtinMethods[Function] = "call apply".split(' ');
    builtinMethods[Object] = "bsonsize".split(' ');

    builtinMethods[Mongo] = "find update insert remove".split(' ');
    builtinMethods[BinData] = "hex base64 length subtype".split(' ');
    builtinMethods[NumberLong] = "toNumber".split(' ');

    var extraGlobals = "Infinity NaN undefined null true false decodeURI decodeURIComponent encodeURI encodeURIComponent escape eval isFinite isNaN parseFloat parseInt unescape Array Boolean Date Math Number RegExp String print load gc MinKey MaxKey Mongo NumberLong ObjectId DBPointer UUID BinData Map".split(' ');

    var isPrivate = function(name){
        if (shellAutocomplete.showPrivate) return false;
        if (name == '_id') return false;
        if (name[0] == '_') return true;
        if (name[name.length-1] == '_') return true; // some native functions have an extra name_ method
        return false;
    }

    var customComplete = function(obj){
        try {
            if(obj.__proto__.constructor.autocomplete){
                var ret = obj.constructor.autocomplete(obj);
                if (ret.constructor != Array){
                    print("\nautocompleters must return real Arrays");
                    return [];
                }
                return ret;
            } else {
                return [];
            }
        } catch (e) {
            // print(e); // uncomment if debugging custom completers
            return [];
        }
    }

    var worker = function( prefix ){
        var global = (function(){return this;}).call(); // trick to get global object

        var curObj = global;
        var parts = prefix.split('.');
        for (var p=0; p < parts.length - 1; p++){ // doesn't include last part
            curObj = curObj[parts[p]];
            if (curObj == null)
                return [];
        }

        var lastPrefix = parts[parts.length-1] || '';
        var begining = parts.slice(0, parts.length-1).join('.');
        if (begining.length)
            begining += '.';

        var possibilities = new Array().concat(
            universalMethods,
            Object.keySet(curObj),
            Object.keySet(curObj.__proto__),
            builtinMethods[curObj] || [], // curObj is a builtin constructor
            builtinMethods[curObj.__proto__.constructor] || [], // curObj is made from a builtin constructor
            curObj == global ? extraGlobals : [],
            customComplete(curObj)
        );

        var ret = [];
        for (var i=0; i < possibilities.length; i++){
            var p = possibilities[i];
            if (typeof(curObj[p]) == "undefined" && curObj != global) continue; // extraGlobals aren't in the global object
            if (p.length == 0 || p.length < lastPrefix.length) continue;
            if (isPrivate(p)) continue;
            if (p.match(/^[0-9]+$/)) continue; // don't array number indexes
            if (p.substr(0, lastPrefix.length) != lastPrefix) continue;

            var completion = begining + p;
            if(curObj[p] && curObj[p].constructor == Function && p != 'constructor')
                completion += '(';

            ret.push(completion);
        }

        return ret;
    }

    // this is the actual function that gets assigned to shellAutocomplete
    return function( prefix ){
        try {
            __autocomplete__ = worker(prefix).sort();
        }catch (e){
            print("exception durring autocomplete: " + tojson(e.message));
            __autocomplete__ = [];
        }
    }
}();

shellAutocomplete.showPrivate = false; // toggle to show (useful when working on internals)

shellHelper = function( command , rest , shouldPrint ){
    command = command.trim();
    var args = rest.trim().replace(/;$/,"").split( "\s+" );
    
    if ( ! shellHelper[command] )
        throw "no command [" + command + "]";
    
    var res = shellHelper[command].apply( null , args );
    if ( shouldPrint ){
        shellPrintHelper( res );
    }
    return res;
}

shellHelper.use = function( dbname ){
    db = db.getMongo().getDB( dbname );
    print( "switched to db " + db.getName() );
}

shellHelper.it = function(){
    if ( typeof( ___it___ ) == "undefined" || ___it___ == null ){
        print( "no cursor" );
        return;
    }
    shellPrintHelper( ___it___ );
}

shellHelper.show = function( what ){
    assert( typeof what == "string" );
    
    if( what == "profile" ) { 
	if( db.system.profile.count() == 0 ) { 
	    print("db.system.profile is empty");
	    print("Use db.setProfilingLevel(2) will enable profiling");
	    print("Use db.system.profile.find() to show raw profile entries");
	} 
	else { 
	    print(); 
	    db.system.profile.find({ millis : { $gt : 0 } }).sort({$natural:-1}).limit(5).forEach( function(x){print(""+x.millis+"ms " + String(x.ts).substring(0,24)); print(x.info); print("\n");} )
        }
	return "";
    }

    if ( what == "users" ){
	db.system.users.find().forEach( printjson );
        return "";
    }

    if ( what == "collections" || what == "tables" ) {
        db.getCollectionNames().forEach( function(x){print(x)} );
	return "";
    }
    
    if ( what == "dbs" ) {
        db.getMongo().getDBNames().sort().forEach( function(x){print(x)} );
	return "";
    }
    
    throw "don't know how to show [" + what + "]";

}

if ( typeof( Map ) == "undefined" ){
    Map = function(){
        this._data = {};
    }
}

Map.hash = function( val ){
    if ( ! val )
        return val;

    switch ( typeof( val ) ){
    case 'string':
    case 'number':
    case 'date':
        return val.toString();
    case 'object':
    case 'array':
        var s = "";
        for ( var k in val ){
            s += k + val[k];
        }
        return s;
    }

    throw "can't hash : " + typeof( val );
}

Map.prototype.put = function( key , value ){
    var o = this._get( key );
    var old = o.value;
    o.value = value;
    return old;
}

Map.prototype.get = function( key ){
    return this._get( key ).value;
}

Map.prototype._get = function( key ){
    var h = Map.hash( key );
    var a = this._data[h];
    if ( ! a ){
        a = [];
        this._data[h] = a;
    }
    
    for ( var i=0; i<a.length; i++ ){
        if ( friendlyEqual( key , a[i].key ) ){
            return a[i];
        }
    }
    var o = { key : key , value : null };
    a.push( o );
    return o;
}

Map.prototype.values = function(){
    var all = [];
    for ( var k in this._data ){
        this._data[k].forEach( function(z){ all.push( z.value ); } );
    }
    return all;
}

if ( typeof( gc ) == "undefined" ){
    gc = function(){
        print( "warning: using noop gc()" );
    }
}
   

Math.sigFig = function( x , N ){
    if ( ! N ){
        N = 3;
    }
    var p = Math.pow( 10, N - Math.ceil( Math.log( Math.abs(x) ) / Math.log( 10 )) );
    return Math.round(x*p)/p;
}

Random = function() {}

// set random seed
Random.srand = function( s ) { _srand( s ); }

// random number 0 <= r < 1
Random.rand = function() { return _rand(); }

// random integer 0 <= r < n
Random.randInt = function( n ) { return Math.floor( Random.rand() * n ); }

Random.setRandomSeed = function( s ) {
    s = s || new Date().getTime();
    print( "setting random seed: " + s );
    Random.srand( s );
}

// generate a random value from the exponential distribution with the specified mean
Random.genExp = function( mean ) {
    return -Math.log( Random.rand() ) * mean;
}

Geo = {};
Geo.distance = function( a , b ){
    var ax = null;
    var ay = null;
    var bx = null;
    var by = null;
    
    for ( var key in a ){
        if ( ax == null )
            ax = a[key];
        else if ( ay == null )
            ay = a[key];
    }
    
    for ( var key in b ){
        if ( bx == null )
            bx = b[key];
        else if ( by == null )
            by = b[key];
    }

    return Math.sqrt( Math.pow( by - ay , 2 ) + 
                      Math.pow( bx - ax , 2 ) );
}

Geo.sphereDistance = function( a , b ){
    var ax = null;
    var ay = null;
    var bx = null;
    var by = null;
    
    // TODO swap order of x and y when done on server
    for ( var key in a ){
        if ( ax == null )
            ax = a[key] * (Math.PI/180);
        else if ( ay == null )
            ay = a[key] * (Math.PI/180);
    }
    
    for ( var key in b ){
        if ( bx == null )
            bx = b[key] * (Math.PI/180);
        else if ( by == null )
            by = b[key] * (Math.PI/180);
    }

    var sin_x1=Math.sin(ax), cos_x1=Math.cos(ax);
    var sin_y1=Math.sin(ay), cos_y1=Math.cos(ay);
    var sin_x2=Math.sin(bx), cos_x2=Math.cos(bx);
    var sin_y2=Math.sin(by), cos_y2=Math.cos(by);

    var cross_prod = 
        (cos_y1*cos_x1 * cos_y2*cos_x2) +
        (cos_y1*sin_x1 * cos_y2*sin_x2) +
        (sin_y1        * sin_y2);

    if (cross_prod >= 1 || cross_prod <= -1){
        // fun with floats
        assert( Math.abs(cross_prod)-1 < 1e-6 );
        return cross_prod > 0 ? 0 : Math.PI;
    }

    return Math.acos(cross_prod);
}

rs = function () { return "try rs.help()"; }

rs.help = function () {
    print("\trs.status()                     { replSetGetStatus : 1 } checks repl set status");
    print("\trs.initiate()                   { replSetInitiate : null } initiates set with default settings");
    print("\trs.initiate(cfg)                { replSetInitiate : cfg } initiates set with configuration cfg");
    print("\trs.add(hostportstr)             add a new member to the set with default attributes");
    print("\trs.add(membercfgobj)            add a new member to the set with extra attributes");
    print("\trs.addArb(hostportstr)          add a new member which is arbiterOnly:true");
    print("\trs.stepDown()                   step down as primary (momentarily)");
    print("\trs.conf()                       return configuration from local.system.replset");
    print("\trs.slaveOk()                    shorthand for db.getMongo().setSlaveOk()");
    print();
    print("\tdb.isMaster()                   check who is primary");
    print();
    print("\tsee also http://<mongod_host>:28017/_replSet for additional diagnostic info");
}
rs.slaveOk = function () { return db.getMongo().setSlaveOk(); }
rs.status = function () { return db._adminCommand("replSetGetStatus"); }
rs.isMaster = function () { return db.isMaster(); }
rs.initiate = function (c) { return db._adminCommand({ replSetInitiate: c }); }
rs.add = function (hostport, arb) {
    var cfg = hostport;

    var local = db.getSisterDB("local");
    assert(local.system.replset.count() <= 1, "error: local.system.replset has unexpected contents");
    var c = local.system.replset.findOne();
    assert(c, "no config object retrievable from local.system.replset");
    c.version++;
    var max = 0;
    for (var i in c.members)
        if (c.members[i]._id > max) max = c.members[i]._id;
    if (isString(hostport)) {
        cfg = { _id: max + 1, host: hostport };
        if (arb)
            cfg.arbiterOnly = true;
    }
    c.members.push(cfg);
    return db._adminCommand({ replSetReconfig: c });
}
rs.stepDown = function () { return db._adminCommand({ replSetStepDown:true}); }
rs.addArb = function (hn) { return this.add(hn, true); }
rs.conf = function () { return db.getSisterDB("local").system.replset.findOne(); }

help = shellHelper.help = function (x) {
    if (x == "connect") {
        print("\nNormally one specifies the server on the mongo shell command line.  Run mongo --help to see those options.");
        print("Additional connections may be opened:\n");
        print("    var x = new Mongo('host[:port]');");
        print("    var mydb = x.getDB('mydb');");
        print("  or");
        print("    var mydb = connect('host[:port]/mydb');");
        print("\nNote: the REPL prompt only auto-reports getLastError() for the shell command line connection.\n");
        return;
    }
    if (x == "misc") {
        print("\tb = new BinData(subtype,base64str)  create a BSON BinData value");
        print("\tb.subtype()                         the BinData subtype (0..255)");
        print("\tb.length()                          length of the BinData data in bytes");
        print("\tb.hex()                             the data as a hex encoded string");
        print("\tb.base64()                          the data as a base 64 encoded string");
        print("\tb.toString()");
        return;
    }
    if (x == "admin") {
        print("\tls([path])                      list files");
        print("\tpwd()                           returns current directory");
        print("\tlistFiles([path])               returns file list");
        print("\thostname()                      returns name of this host");
        print("\tcat(fname)                      returns contents of text file as a string");
        print("\tremoveFile(f)                   delete a file");
        print("\tload(jsfilename)                load and execute a .js file");
        print("\trun(program[, args...])         spawn a program and wait for its completion");
        print("\tsleep(m)                        sleep m milliseconds");
        print("\tgetMemInfo()                    diagnostic");
        return;
    }
    if (x == "test") {
        print("\tstartMongodEmpty(args)        DELETES DATA DIR and then starts mongod");
        print("\t                              returns a connection to the new server");
        print("\tstartMongodTest()             DELETES DATA DIR");
        print("\t                              automatically picks port #s starting at 27000 and increasing");
        print("\t                              or you can specify the port as the first arg");
        print("\t                              dir is /data/db/<port>/ if not specified as the 2nd arg");
        print("\t                              returns a connection to the new server");
        return;
    }
    print("\t" + "db.help()                    help on db methods");
    print("\t" + "db.mycoll.help()             help on collection methods");
    print("\t" + "rs.help()                    help on replica set methods");
    print("\t" + "help connect                 connecting to a db help");
    print("\t" + "help admin                   administrative help");
    print("\t" + "help misc                    misc things to know");
    print();
    print("\t" + "show dbs                     show database names");
    print("\t" + "show collections             show collections in current database");
    print("\t" + "show users                   show users in current database");
    print("\t" + "show profile                 show most recent system.profile entries with time >= 1ms");
    print("\t" + "use <db_name>                set current database");
    print("\t" + "db.foo.find()                list objects in collection foo");
    print("\t" + "db.foo.find( { a : 1 } )     list objects in foo where a == 1");
    print("\t" + "it                           result of the last line evaluated; use to further iterate");
    print("\t" + "DBQuery.shellBatchSize = x   set default number of items to display on shell" );
    print("\t" + "exit                         quit the mongo shell");
}
