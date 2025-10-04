// Tests to see what validity checks are done for 10gen specific object construction
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_non_retryable_commands,
//   uses_map_reduce_with_temp_collections,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   requires_scripting,
// ]

// Takes a list of constructors and returns a new list with an extra entry for each constructor with
// "new" prepended
const out = db.map_reduce_constructors_out;
function addConstructorsWithNew(constructorList) {
    function prependNew(constructor) {
        return "new " + constructor;
    }

    let valid = constructorList.valid;
    let invalid = constructorList.invalid;
    // We use slice(0) here to make a copy of our lists
    let validWithNew = valid.concat(valid.slice(0).map(prependNew));
    let invalidWithNew = invalid.concat(invalid.slice(0).map(prependNew));
    return {"valid": validWithNew, "invalid": invalidWithNew};
}

function clientEvalConstructorTest(constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    constructorList.valid.forEach(function (constructor) {
        try {
            eval(constructor);
        } catch (e) {
            throw "valid constructor: " + constructor + " failed in eval context: " + e;
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        assert.throws(
            function () {
                eval(constructor);
            },
            [],
            "invalid constructor did not throw error in eval context: " + constructor,
        );
    });
}

function mapReduceConstructorTest(constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    const t = db.mr_constructors;
    t.drop();

    t.save({"partner": 1, "visits": 9});
    t.save({"partner": 2, "visits": 9});
    t.save({"partner": 1, "visits": 11});
    t.save({"partner": 1, "visits": 30});
    t.save({"partner": 2, "visits": 41});
    t.save({"partner": 2, "visits": 41});

    let dummy;
    constructorList.valid.forEach(function (constructor) {
        try {
            const m = eval('dummy = function(){ emit( "test" , ' + constructor + " ) }");

            const r = eval("dummy = function( k , v ){ return { test : " + constructor + " } }");

            out.drop();
            assert.commandWorked(t.mapReduce(m, r, {out: {merge: "map_reduce_constructors_out"}, scope: {xx: 1}}));
        } catch (e) {
            throw "valid constructor: " + constructor + " failed in mapReduce context: " + e;
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        const m = eval('dummy = function(){ emit( "test" , ' + constructor + " ) }");

        const r = eval("dummy = function( k , v ){ return { test : " + constructor + " } }");

        assert.throws(
            function () {
                out.drop();
                t.mapReduce(m, r, {out: {merge: "map_reduce_constructors_out"}, scope: {xx: 1}});
            },
            [],
            "invalid constructor did not throw error in mapReduce context: " + constructor,
        );
    });

    out.drop();
    t.drop();
}

function whereConstructorTest(constructorList) {
    constructorList = addConstructorsWithNew(constructorList);
    const t = db.where_constructors;
    t.drop();
    assert.commandWorked(t.insert({x: 1}));

    constructorList.valid.forEach(function (constructor) {
        try {
            t.findOne({$where: constructor});
        } catch (e) {
            throw "valid constructor: " + constructor + " failed in $where query: " + e;
        }
    });
    constructorList.invalid.forEach(function (constructor) {
        assert.throws(
            function () {
                t.findOne({$where: constructor});
            },
            [],
            "invalid constructor did not throw error in $where query: " + constructor,
        );
    });
}

let dbrefConstructors = {
    "valid": [
        'DBRef("namespace", 0)',
        'DBRef("namespace", "test")',
        'DBRef("namespace", "test", "database")',
        'DBRef("namespace", ObjectId())',
        'DBRef("namespace", ObjectId("000000000000000000000000"))',
        'DBRef("namespace", ObjectId("000000000000000000000000"), "database")',
    ],
    "invalid": [
        "DBRef()",
        "DBRef(true, ObjectId())",
        "DBRef(true, ObjectId(), true)",
        'DBRef("namespace")',
        'DBRef("namespace", ObjectId(), true)',
        'DBRef("namespace", ObjectId(), 123)',
    ],
};

let dbpointerConstructors = {
    "valid": ['DBPointer("namespace", ObjectId())', 'DBPointer("namespace", ObjectId("000000000000000000000000"))'],
    "invalid": [
        "DBPointer()",
        "DBPointer(true, ObjectId())",
        'DBPointer("namespace", 0)',
        'DBPointer("namespace", "test")',
        'DBPointer("namespace")',
        'DBPointer("namespace", ObjectId(), true)',
    ],
};

let objectidConstructors = {
    "valid": ["ObjectId()", 'ObjectId("FFFFFFFFFFFFFFFFFFFFFFFF")'],
    "invalid": ["ObjectId(5)", 'ObjectId("FFFFFFFFFFFFFFFFFFFFFFFQ")'],
};

let timestampConstructors = {
    "valid": ["Timestamp()", "Timestamp(0,0)", "Timestamp(1.0,1.0)"],
    "invalid": [
        "Timestamp(0)",
        "Timestamp(0,0,0)",
        'Timestamp("test","test")',
        'Timestamp("test",0)',
        'Timestamp(0,"test")',
        "Timestamp(true,true)",
        "Timestamp(true,0)",
        "Timestamp(0,true)",
        "Timestamp(Math.pow(2,32),Math.pow(2,32))",
        "Timestamp(0,Math.pow(2,32))",
        "Timestamp(Math.pow(2,32),0)",
        "Timestamp(-1,-1)",
        "Timestamp(-1,0)",
        "Timestamp(0,-1)",
    ],
};

let bindataConstructors = {
    "valid": ['BinData(0,"test")'],
    "invalid": [
        'BinData(0,"test", "test")',
        "BinData()",
        'BinData(-1, "")',
        'BinData(256, "")',
        'BinData("string","aaaa")',
        "BinData(0, true)",
        "BinData(0, null)",
        "BinData(0, undefined)",
        "BinData(0, {})",
        "BinData(0, [])",
        "BinData(0, function () {})",
    ],
};

let uuidConstructors = {
    "valid": [
        'UUID("0123456789abcdef0123456789ABCDEF")',
        'UUID("0a1A2b3B-4c5C-6d7D-8e9E-AfBFC0D1E3F4")',
        'UUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")',
        "UUID()",
        UUID().toString(),
    ],
    "invalid": [
        'UUID("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)',
        'UUID("aaaaaaaa-aaaa-aaaa-aaaaaaaa-aaaaaaaa")',
        'UUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaa-aaaaaaa")',
        'UUID("aa")',
        'UUID("invalidhex")',
        'UUID("invalidhexbutstilltherequiredlen")',
        "UUID(true)",
        "UUID(null)",
        "UUID(undefined)",
        "UUID({})",
        "UUID([])",
        "UUID(function () {})",
    ],
};

let md5Constructors = {
    "valid": ['MD5("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")'],
    "invalid": [
        'MD5("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)',
        "MD5()",
        'MD5("aa")',
        'MD5("invalidhex")',
        'MD5("invalidhexbutstilltherequiredlen")',
        "MD5(true)",
        "MD5(null)",
        "MD5(undefined)",
        "MD5({})",
        "MD5([])",
        "MD5(function () {})",
    ],
};

let hexdataConstructors = {
    "valid": [
        'HexData(0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")',
        'HexData(0, "")',
        'HexData(0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")',
        'HexData(0, "000000000000000000000005")', // SERVER-9605
    ],
    "invalid": [
        'HexData(0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0)',
        "HexData()",
        "HexData(0)",
        "HexData(0, 100)",
        'HexData(-1, "")',
        'HexData(256, "")',
        'HexData(0, "aaa")',
        'HexData("string","aaaa")',
        "HexData(0, true)",
        "HexData(0, null)",
        "HexData(0, undefined)",
        "HexData(0, {})",
        "HexData(0, [])",
        "HexData(0, function () {})",
        'HexData(0, "invalidhex")',
    ],
};

let dateConstructors = {
    "valid": ["Date()", "Date(0)", "Date(0,0)", "Date(0,0,0)", 'Date("foo")'],
    "invalid": [],
};

clientEvalConstructorTest(dbrefConstructors);
clientEvalConstructorTest(dbpointerConstructors);
clientEvalConstructorTest(objectidConstructors);
clientEvalConstructorTest(timestampConstructors);
clientEvalConstructorTest(bindataConstructors);
clientEvalConstructorTest(uuidConstructors);
clientEvalConstructorTest(md5Constructors);
clientEvalConstructorTest(hexdataConstructors);
clientEvalConstructorTest(dateConstructors);

mapReduceConstructorTest(dbrefConstructors);
mapReduceConstructorTest(dbpointerConstructors);
mapReduceConstructorTest(objectidConstructors);
mapReduceConstructorTest(timestampConstructors);
mapReduceConstructorTest(bindataConstructors);
mapReduceConstructorTest(uuidConstructors);
mapReduceConstructorTest(md5Constructors);
mapReduceConstructorTest(hexdataConstructors);
mapReduceConstructorTest(dateConstructors);

whereConstructorTest(dbrefConstructors);
whereConstructorTest(dbpointerConstructors);
whereConstructorTest(objectidConstructors);
whereConstructorTest(timestampConstructors);
whereConstructorTest(bindataConstructors);
whereConstructorTest(uuidConstructors);
whereConstructorTest(md5Constructors);
whereConstructorTest(hexdataConstructors);
whereConstructorTest(dateConstructors);
