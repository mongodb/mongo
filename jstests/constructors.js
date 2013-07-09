// Tests to see what validity checks are done for 10gen specific object construction

// Takes a list of constructors and returns a new list with an extra entry for each constructor with
// "new" prepended
function addConstructorsWithNew (constructorList) {
    function prependNew (constructor) {
        return "new " + constructor;
    }

    var valid = constructorList.valid;
    var invalid = constructorList.invalid;
    // We use slice(0) here to make a copy of our lists
    var validWithNew = valid.concat(valid.slice(0).map(prependNew));
    var invalidWithNew = invalid.concat(invalid.slice(0).map(prependNew));
    return { "valid" : validWithNew, "invalid" : invalidWithNew };
}

function clientEvalConstructorTest (constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    constructorList.valid.forEach(function (constructor) {
        try {
            eval(constructor);
        }
        catch (e) {
            throw ("valid constructor: " + constructor + " failed in eval context: " + e);
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        assert.throws(function () { eval(constructor) },
                      [], "invalid constructor did not throw error in eval context: " + constructor);
    });
}

function dbEvalConstructorTest (constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    constructorList.valid.forEach(function (constructor) {
        try {
            db.eval(constructor);
        }
        catch (e) {
            throw ("valid constructor: " + constructor + " failed in db.eval context: " + e);
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        assert.throws(function () { db.eval(constructor) },
                      [], "invalid constructor did not throw error in db.eval context: " + constructor);
    });
}

function mapReduceConstructorTest (constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    t = db.mr_constructors;
    t.drop();

    t.save( { "partner" : 1, "visits" : 9 } )
    t.save( { "partner" : 2, "visits" : 9 } )
    t.save( { "partner" : 1, "visits" : 11 } )
    t.save( { "partner" : 1, "visits" : 30 } )
    t.save( { "partner" : 2, "visits" : 41 } )
    t.save( { "partner" : 2, "visits" : 41 } )

    constructorList.valid.forEach(function (constructor) {
        try {
            m = eval("dummy = function(){ emit( \"test\" , " + constructor + " ) }");

            r = eval("dummy = function( k , v ){ return { test : " + constructor + " } }");

            res = t.mapReduce( m , r , { out : "mr_constructors_out" , scope : { xx : 1 } } );
        }
        catch (e) {
            throw ("valid constructor: " + constructor + " failed in mapReduce context: " + e);
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        m = eval("dummy = function(){ emit( \"test\" , " + constructor + " ) }");

        r = eval("dummy = function( k , v ){ return { test : " + constructor + " } }");

        assert.throws(function () { res = t.mapReduce( m , r ,
                                    { out : "mr_constructors_out" , scope : { xx : 1 } } ) },
                      [], "invalid constructor did not throw error in mapReduce context: " + constructor);
    });

    db.mr_constructors_out.drop();
    t.drop();
}

function whereConstructorTest (constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    t = db.where_constructors;
    t.drop();
    t.insert({ x : 1 });
    assert(!db.getLastError());

    constructorList.valid.forEach(function (constructor) {
        try {
            t.findOne({ $where : constructor });
        }
        catch (e) {
            throw ("valid constructor: " + constructor + " failed in $where query: " + e);
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        assert.throws(function () { t.findOne({ $where : constructor }) },
                      [], "invalid constructor did not throw error in $where query: " + constructor);
    });
}

var dbrefConstructors = {
    "valid" : [
            "DBRef(\"namespace\", 0)",
            "DBRef(\"namespace\", \"test\")",
            "DBRef(\"namespace\", ObjectId())",
            "DBRef(\"namespace\", ObjectId(\"000000000000000000000000\"))",
        ],
    "invalid" : [
            "DBRef()",
            "DBRef(true, ObjectId())",
            "DBRef(\"namespace\")",
            "DBRef(\"namespace\", ObjectId(), true)",
        ]
}

var dbpointerConstructors = {
    "valid" : [
            "DBPointer(\"namespace\", ObjectId())",
            "DBPointer(\"namespace\", ObjectId(\"000000000000000000000000\"))",
        ],
    "invalid" : [
            "DBPointer()",
            "DBPointer(true, ObjectId())",
            "DBPointer(\"namespace\", 0)",
            "DBPointer(\"namespace\", \"test\")",
            "DBPointer(\"namespace\")",
            "DBPointer(\"namespace\", ObjectId(), true)",
        ]
}


var objectidConstructors = {
    "valid" : [
        'ObjectId()',
        'ObjectId("FFFFFFFFFFFFFFFFFFFFFFFF")',
        ],
    "invalid" : [
        'ObjectId(5)',
        'ObjectId("FFFFFFFFFFFFFFFFFFFFFFFQ")',
        ]
}

var timestampConstructors = {
    "valid" : [
        'Timestamp()',
        'Timestamp(0,0)',
        'Timestamp(1.0,1.0)',
        ],
    "invalid" : [
        'Timestamp(0)',
        'Timestamp(0,0,0)',
        'Timestamp("test","test")',
        'Timestamp("test",0)',
        'Timestamp(0,"test")',
        'Timestamp(true,true)',
        'Timestamp(true,0)',
        'Timestamp(0,true)',
        ]
}

var bindataConstructors = {
    "valid" : [
        'BinData(0,"test")',
        ],
    "invalid" : [
        'BinData(0,"test", "test")',
        'BinData()',
        'BinData(-1, "")',
        'BinData(256, "")',
        'BinData("string","aaaa")',
        // SERVER-10152
        //'BinData(0, true)',
        //'BinData(0, null)',
        //'BinData(0, undefined)',
        //'BinData(0, {})',
        //'BinData(0, [])',
        //'BinData(0, function () {})',
        ]
}

var uuidConstructors = {
    "valid" : [
        'UUID("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")',
        ],
    "invalid" : [
        'UUID("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)',
        'UUID()',
        'UUID("aa")',
        'UUID("invalidhex")',
        // SERVER-9686
        //'UUID("invalidhexbutstilltherequiredlen")',
        'UUID(true)',
        'UUID(null)',
        'UUID(undefined)',
        'UUID({})',
        'UUID([])',
        'UUID(function () {})',
        ]
}

var md5Constructors = {
    "valid" : [
        'MD5("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")',
        ],
    "invalid" : [
        'MD5("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)',
        'MD5()',
        'MD5("aa")',
        'MD5("invalidhex")',
        // SERVER-9686
        //'MD5("invalidhexbutstilltherequiredlen")',
        'MD5(true)',
        'MD5(null)',
        'MD5(undefined)',
        'MD5({})',
        'MD5([])',
        'MD5(function () {})',
        ]
}

var hexdataConstructors = {
    "valid" : [
        'HexData(0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")',
        // Numbers as the payload are converted to strings, so HexData(0, 100) == HexData(0, "100")
        'HexData(0, 100)',
        'HexData(0, "")',
        'HexData(0, "aaa")',
        'HexData(0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")',
        'HexData(0, "000000000000000000000005")', // SERVER-9605
        ],
    "invalid" : [
        'HexData(0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)',
        'HexData()',
        'HexData(0)',
        'HexData(-1, "")',
        'HexData(256, "")',
        'HexData("string","aaaa")',
        // SERVER-10152
        //'HexData(0, true)',
        //'HexData(0, null)',
        //'HexData(0, undefined)',
        //'HexData(0, {})',
        //'HexData(0, [])',
        //'HexData(0, function () {})',
        // SERVER-9686
        //'HexData(0, "invalidhex")',
        ]
}

var dateConstructors = {
    "valid" : [
        'Date()',
        'Date(0)',
        'Date(0,0)',
        'Date(0,0,0)',
        'Date("foo")',
        ],
    "invalid" : [
        ]
}

clientEvalConstructorTest(dbrefConstructors);
clientEvalConstructorTest(dbpointerConstructors);
clientEvalConstructorTest(objectidConstructors);
clientEvalConstructorTest(timestampConstructors);
clientEvalConstructorTest(bindataConstructors);
clientEvalConstructorTest(uuidConstructors);
clientEvalConstructorTest(md5Constructors);
clientEvalConstructorTest(hexdataConstructors);
clientEvalConstructorTest(dateConstructors);

dbEvalConstructorTest(dbrefConstructors);
dbEvalConstructorTest(dbpointerConstructors);
dbEvalConstructorTest(objectidConstructors);
dbEvalConstructorTest(timestampConstructors);
dbEvalConstructorTest(bindataConstructors);
dbEvalConstructorTest(uuidConstructors);
dbEvalConstructorTest(md5Constructors);
dbEvalConstructorTest(hexdataConstructors);
dbEvalConstructorTest(dateConstructors);

// SERVER-8963
if (db.runCommand({buildinfo:1}).javascriptEngine == "V8") {
    mapReduceConstructorTest(dbrefConstructors);
    mapReduceConstructorTest(dbpointerConstructors);
    mapReduceConstructorTest(objectidConstructors);
    mapReduceConstructorTest(timestampConstructors);
    mapReduceConstructorTest(bindataConstructors);
    mapReduceConstructorTest(uuidConstructors);
    mapReduceConstructorTest(md5Constructors);
    mapReduceConstructorTest(hexdataConstructors);
}
mapReduceConstructorTest(dateConstructors);

// SERVER-8963
if (db.runCommand({buildinfo:1}).javascriptEngine == "V8") {
    whereConstructorTest(dbrefConstructors);
    whereConstructorTest(dbpointerConstructors);
    whereConstructorTest(objectidConstructors);
    whereConstructorTest(timestampConstructors);
    whereConstructorTest(bindataConstructors);
    whereConstructorTest(uuidConstructors);
    whereConstructorTest(md5Constructors);
    whereConstructorTest(hexdataConstructors);
}
whereConstructorTest(dateConstructors);
