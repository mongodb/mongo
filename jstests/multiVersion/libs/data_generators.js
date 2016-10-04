// This file contains test helpers to generate streams of various types of data.

//
// Generates a stream of normal documents, with all the different BSON types that are representable
// from the shell.
//
// Interface:
//
// next() // Get the next document in the stream
// hasNext() // Check if the stream has any more documents
//
// Usage:
//
// var generator = new DataGenerator();
// while (generator.hasNext()) {
//     var nextDoc = generator.next();
//     // Do something with nextDoc
// }
//
function DataGenerator() {
    var hexChars = "0123456789abcdefABCDEF";
    var regexOptions = "igm";
    var stringChars = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    var base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Generator functions
    // BSON Type: -1
    function GenMinKey(seed) {
        return MinKey();
    }
    // BSON Type: 0 (EOO)
    // No Shell Equivalent
    // BSON Type: 1
    function GenNumberDouble(seed) {
        var seed = seed || 0;
        return Number(seed);
    }
    // BSON Type: 2
    function GenString(seed) {
        var seed = seed || 0;
        var text = "";

        for (var i = 0; i < (seed % 1000) + 1; i++) {
            text += stringChars.charAt((seed + (i % 10)) % stringChars.length);
        }

        return text;
    }
    // Javascript Dates get stored as strings
    function GenDate(seed) {
        // The "Date" constructor without "new" ignores its arguments anyway, so don't bother
        // using the seed.
        return Date();
    }
    // BSON Type: 3
    function GenObject(seed) {
        var seed = seed || 0;

        return {"object": true};
    }
    // BSON Type: 4
    function GenArray(seed) {
        var seed = seed || 0;

        return ["array", true];
    }
    // BSON Type: 5
    function GenBinData(seed) {
        var seed = seed || 0;

        var text = "";

        for (var i = 0; i < (seed % 1000) + 1; i++) {
            text += base64Chars.charAt((seed + (i % 10)) % base64Chars.length);
        }

        while ((text.length % 4) != 0) {
            text += "=";
        }

        return BinData(seed % 6, text);
    }
    // BSON Type: 6
    function GenUndefined(seed) {
        return undefined;
    }
    // BSON Type: 7
    function GenObjectId(seed) {
        var seed = seed || 0;
        var hexString = "";

        for (var i = 0; i < 24; i++) {
            hexString += hexChars.charAt((seed + (i % 10)) % hexChars.length);
        }

        return ObjectId(hexString);
    }
    // BSON Type: 8
    function GenBool(seed) {
        var seed = seed || 0;

        return (seed % 2) === 0;
    }
    // BSON Type: 9
    // Our ISODate constructor equals the Date BSON type
    function GenISODate(seed) {
        var seed = seed || 0;

        var year = (seed % (2037 - 1970)) + 1970;
        var month = (seed % 12) + 1;
        var day = (seed % 27) + 1;
        var hour = seed % 24;
        var minute = seed % 60;
        var second = seed % 60;
        var millis = seed % 1000;

        function pad(number, length) {
            var str = '' + number;

            while (str.length < length) {
                str = '0' + str;
            }

            return str;
        }

        return ISODate(pad(year, 4) + "-" + pad(month, 2) + "-" + pad(day, 2) + "T" + pad(hour, 2) +
                       ":" + pad(minute, 2) + ":" + pad(second, 2) + "." + pad(millis, 3));
    }
    // BSON Type: 10
    function GenNull(seed) {
        return null;
    }
    // BSON Type: 11
    function GenRegExp(seed) {
        var seed = seed || 0;
        var options = "";

        for (var i = 0; i < (seed % 3) + 1; i++) {
            options += regexOptions.charAt((seed + (i % 10)) % regexOptions.length);
        }

        return RegExp(GenString(seed), options);
    }
    function GenRegExpLiteral(seed) {
        // We can't pass variables to a regex literal, so we can't programmatically generate the
        // data.  Instead we rely on the "RegExp" constructor.
        return /regexliteral/;
    }
    // BSON Type: 12
    // The DBPointer type in the shell equals the DBRef BSON type
    function GenDBPointer(seed) {
        var seed = seed || 0;

        return DBPointer(GenString(seed), GenObjectId(seed));
    }
    // BSON Type: 13 (Code)
    // No Shell Equivalent
    // BSON Type: 14 (Symbol)
    // No Shell Equivalent
    // BSON Type: 15 (CodeWScope)
    // No Shell Equivalent
    // BSON Type: 16
    function GenNumberInt(seed) {
        var seed = seed || 0;

        return NumberInt(seed);
    }
    // BSON Type: 17
    function GenTimestamp(seed) {
        var seed = seed || 0;

        // Make sure our timestamp is not zero, because that doesn't round trip from 2.4 to latest.
        // See SERVER-12302.
        if (seed == 0) {
            seed = 1;
        }

        return Timestamp(seed, (seed * 100000) / 99999);
    }
    // BSON Type: 18
    function GenNumberLong(seed) {
        var seed = seed || 0;

        return NumberLong(seed);
    }
    // BSON Type: 127
    function GenMaxKey(seed) {
        return MaxKey();
    }
    // The DBRef type is not a BSON type but is treated specially in the shell:
    function GenDBRef(seed) {
        var seed = seed || 0;

        return DBRef(GenString(seed), GenObjectId(seed));
    }

    function GenFlatObjectAllTypes(seed) {
        return {
            "MinKey": GenMinKey(seed),
            "NumberDouble": GenNumberDouble(seed),
            "String": GenString(seed),
            // Javascript Dates get stored as strings
            "Date": GenDate(seed),
            // BSON Type: 3
            "Object": GenObject(seed),
            // BSON Type: 4
            "Array": GenArray(seed),
            // BSON Type: 5
            "BinData": GenBinData(seed),
            // BSON Type: 6
            "Undefined": undefined,
            // BSON Type: 7
            "jstOID": GenObjectId(seed),
            // BSON Type: 8
            "Bool": GenBool(seed),
            // BSON Type: 9
            // Our ISODate constructor equals the Date BSON type
            "ISODate": GenISODate(seed),
            // BSON Type: 10
            "jstNULL": GenNull(seed),
            // BSON Type: 11
            "RegExp": GenRegExp(seed),
            "RegExpLiteral": GenRegExpLiteral(seed),
            // BSON Type: 12
            // The DBPointer type in the shell equals the DBRef BSON type
            "DBPointer": GenDBPointer(seed),
            // BSON Type: 13 (Code)
            // No Shell Equivalent
            // BSON Type: 14 (Symbol)
            // No Shell Equivalent
            // BSON Type: 15 (CodeWScope)
            // No Shell Equivalent
            // BSON Type: 16
            "NumberInt": GenNumberInt(seed),
            // BSON Type: 17
            "Timestamp": GenTimestamp(seed),
            // BSON Type: 18
            "NumberLong": GenNumberLong(seed),
            // BSON Type: 127
            "MaxKey": GenMaxKey(seed),
            // The DBRef type is not a BSON type but is treated specially in the shell:
            "DBRef": GenDBRef(seed),
        };
    }

    function GenFlatObjectAllTypesHardCoded() {
        return {
            // BSON Type: -1
            "MinKey": MinKey(),
            // BSON Type: 0 (EOO)
            // No Shell Equivalent
            // BSON Type: 1
            "NumberDouble": Number(4.0),
            // BSON Type: 2
            "String": "string",
            // Javascript Dates get stored as strings
            "Date": Date("2013-12-11T19:38:24.055Z"),
            "Date2": GenDate(10000),
            // BSON Type: 3
            "Object": {"object": true},
            // BSON Type: 4
            "Array": ["array", true],
            // BSON Type: 5
            "BinData": BinData(0, "aaaa"),
            // BSON Type: 6
            "Undefined": undefined,
            // BSON Type: 7
            "jstOID": ObjectId("aaaaaaaaaaaaaaaaaaaaaaaa"),
            // BSON Type: 8
            "Bool": true,
            // BSON Type: 9
            // Our ISODate constructor equals the Date BSON type
            "ISODate": ISODate("2013-12-11T19:38:24.055Z"),
            // BSON Type: 10
            "jstNULL": null,
            // BSON Type: 11
            "RegExp": RegExp("a"),
            "RegExpLiteral": /a/,
            // BSON Type: 12
            // The DBPointer type in the shell equals the DBRef BSON type
            "DBPointer": DBPointer("foo", ObjectId("bbbbbbbbbbbbbbbbbbbbbbbb")),
            // BSON Type: 13 (Code)
            // No Shell Equivalent
            // BSON Type: 14 (Symbol)
            // No Shell Equivalent
            // BSON Type: 15 (CodeWScope)
            // No Shell Equivalent
            // BSON Type: 16
            "NumberInt": NumberInt(5),
            // BSON Type: 17
            "Timestamp": Timestamp(1, 2),
            // BSON Type: 18
            "NumberLong": NumberLong(6),
            // BSON Type: 127
            "MaxKey": MaxKey(),
            // The DBRef type is not a BSON type but is treated specially in the shell:
            "DBRef": DBRef("bar", 2)
        };
    }

    // Data we are using as a source for our testing
    testData = [
        GenFlatObjectAllTypesHardCoded(),
        GenFlatObjectAllTypes(0),
        GenFlatObjectAllTypes(2),
        GenFlatObjectAllTypes(3),
        GenFlatObjectAllTypes(5),
        GenFlatObjectAllTypes(7),
        GenFlatObjectAllTypes(23),
        GenFlatObjectAllTypes(111),
        GenFlatObjectAllTypes(11111111),
    ];

    // Cursor interface
    var i = 0;
    return {
        "hasNext": function() {
            return i < testData.length;
        },
        "next": function() {
            if (i >= testData.length) {
                return undefined;
            }
            return testData[i++];
        }
    };
}

//
// Generates a stream of index data documents, with a few different attributes that are valid for
// the different index types.
//
// Interface:
//
// next() // Get the next document in the stream
// hasNext() // Check if the stream has any more documents
//
// Returns documents of the form:
//
// {
//     "spec" : <index spec>,
//     "options" : <index options>
// }
//
// Usage:
//
// var generator = new IndexDataGenerator();
// while (generator.hasNext()) {
//     var nextIndexDocument = generator.next();
//     var nextIndexSpec = nextIndexDocument["spec"];
//     var nextIndexOptions = nextIndexDocument["options"];
//     db.ensureIndex(nextIndexSpec, nextIndexOptions);
// }
//
function IndexDataGenerator(options) {
    // getNextUniqueKey()
    //
    // This function returns a new key each time it is called and is guaranteed to not return
    // duplicates.
    //
    // The sequence of values returned is a-z then A-Z.  When "Z" is reached, a new character is
    // added
    // and the first one wraps around, resulting in "aa".  The process is repeated, so we get a
    // sequence
    // like this:
    //
    // "a"
    // "b"
    // ...
    // "z"
    // "A"
    // ...
    // "Z"
    // "aa"
    // "ba"
    // ...
    var currentKey = "";
    function getNextUniqueKey() {
        function setCharAt(str, index, chr) {
            if (index > str.length - 1) {
                return str;
            }
            return str.substr(0, index) + chr + str.substr(index + 1);
        }

        // Index of the letter we are advancing in our current key
        var currentKeyIndex = 0;
        var keyChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

        do {
            // If we are advancing a letter that does not exist yet, append a new character and
            // return the result.  For example, this is the case where "aa" follows "Z".
            if (currentKeyIndex + 1 > currentKey.length) {
                currentKey += keyChars[0];
                return currentKey;
            }

            // Find the character (index into keyChars) that we currently have at this position, set
            // this position to the next character in the keyChars sequence
            keyCharsIndex = keyChars.search(currentKey[currentKeyIndex]);
            currentKey = setCharAt(
                currentKey, currentKeyIndex, keyChars[(keyCharsIndex + 1) % keyChars.length]);
            currentKeyIndex = currentKeyIndex + 1;

            // Loop again if we advanced the character past the end of keyChars and wrapped around,
            // so that we can advance the next character over too.  For example, this is the case
            // where "ab" follows "Za".
        } while (keyCharsIndex + 1 >= keyChars.length);

        return currentKey;
    }

    //
    // Index Generation
    //

    function GenSingleFieldIndex(seed) {
        var index = {};
        index[getNextUniqueKey()] = (seed % 2) == 1 ? 1 : -1;
        return index;
    }

    function GenCompoundIndex(seed) {
        var index = {};
        var i;
        for (i = 0; i < (seed % 2) + 2; i++) {
            index[getNextUniqueKey()] = ((seed + i) % 2) == 1 ? 1 : -1;
        }
        return index;
    }

    function GenNestedIndex(seed) {
        var index = {};
        var i;
        var key = getNextUniqueKey();
        for (i = 0; i < (seed % 2) + 1; i++) {
            key += "." + getNextUniqueKey();
        }
        index[key] = (seed % 2) == 1 ? 1 : -1;
        return index;
    }

    function Gen2dsphereIndex(seed) {
        var index = {};
        index[getNextUniqueKey()] = "2dsphere";
        return index;
    }

    function Gen2dIndex(seed) {
        var index = {};
        index[getNextUniqueKey()] = "2d";
        return index;
    }

    function GenHaystackIndex(seed) {
        var index = {};
        index[getNextUniqueKey()] = "geoHaystack";
        // Haystack indexes need a non geo field, and the geo field must be first
        index[getNextUniqueKey()] = (seed % 2) == 1 ? 1 : -1;
        return index;
    }

    function GenTextIndex(seed) {
        var index = {};
        index[getNextUniqueKey()] = "text";
        return index;
    }

    function GenHashedIndex(seed) {
        var index = {};
        index[getNextUniqueKey()] = "hashed";
        return index;
    }

    function GenIndexOptions(seed) {
        var attributes = {};
        var i;
        for (i = 0; i < (seed % 2) + 1; i++) {
            // Mod 3 first to make sure the property type doesn't correlate with (seed % 2)
            var propertyType = (seed % 3 + i) % 2;
            if (propertyType == 0) {
                attributes["expireAfterSeconds"] = ((seed + i) * 10000) % 9999 + 1000;
            }
            if (propertyType == 1) {
                attributes["sparse"] = true;
            } else {
                // TODO:  We have to test this as a separate stage because we want to round trip
                // multiple documents
                // attributes["unique"] = true;
            }
        }
        return attributes;
    }

    function Gen2dIndexOptions(seed) {
        var attributes = GenIndexOptions(seed);
        var i;
        for (i = 0; i < (seed % 2) + 1; i++) {
            // Mod 3 first to make sure the property type doesn't correlate with (seed % 2)
            var propertyType = (seed + i) % 3;
            // When using a 2d index, the following additional index properties are supported:
            // { "bits" : <bit precision>, "min" : <lower bound>, "max" : <upper bound> }
            if (propertyType == 0) {
                attributes["bits"] = ((seed + i) * 10000) % 100 + 10;
            }
            if (propertyType == 1) {
                attributes["min"] = ((seed + i) * 10000) % 100 + 10;
            }
            if (propertyType == 2) {
                attributes["max"] = ((seed + i) * 10000) % 100 + 10;
            } else {
            }
        }
        // The region specified in a 2d index must be positive
        if (attributes["min"] >= attributes["max"]) {
            attributes["max"] = attributes["min"] + attributes["max"];
        }
        return attributes;
    }

    function GenHaystackIndexOptions(seed) {
        var attributes = GenIndexOptions(seed);
        // When using a haystack index, the following additional index properties are required:
        // { "bucketSize" : <bucket value> }
        attributes["bucketSize"] = (seed * 10000) % 100 + 10;
        return attributes;
    }

    function GenTextIndexOptions(seed) {
        var attributes = GenIndexOptions(seed);
        // When using a text index, the following additional index properties are required when
        // downgrading from 2.6:
        // { "textIndexVersion" : 1 }
        attributes["textIndexVersion"] = 1;
        return attributes;
    }

    function Gen2dSphereIndexOptions(seed) {
        var attributes = GenIndexOptions(seed);
        // When using a 2dsphere index, the following additional index properties are required when
        // downgrading from 2.6:
        // { "2dsphereIndexVersion" : 1 }
        attributes["2dsphereIndexVersion"] = 1;
        return attributes;
    }

    testIndexes = [
        // Single Field Indexes
        {"spec": GenSingleFieldIndex(1), "options": GenIndexOptions(0)},
        {"spec": GenSingleFieldIndex(0), "options": GenIndexOptions(1)},

        // Compound Indexes
        {"spec": GenCompoundIndex(0), "options": GenIndexOptions(2)},
        {"spec": GenCompoundIndex(1), "options": GenIndexOptions(3)},
        {"spec": GenCompoundIndex(2), "options": GenIndexOptions(4)},
        {"spec": GenCompoundIndex(3), "options": GenIndexOptions(5)},
        {"spec": GenCompoundIndex(4), "options": GenIndexOptions(6)},
        {"spec": GenCompoundIndex(5), "options": GenIndexOptions(7)},
        {"spec": GenCompoundIndex(6), "options": GenIndexOptions(8)},

        // Multikey Indexes
        // (Same index spec as single field)

        // Nested Indexes
        {"spec": GenNestedIndex(0), "options": GenIndexOptions(9)},
        {"spec": GenNestedIndex(1), "options": GenIndexOptions(10)},
        {"spec": GenNestedIndex(2), "options": GenIndexOptions(11)},

        // Geospatial Indexes
        //   2dsphere
        {"spec": Gen2dsphereIndex(7), "options": Gen2dSphereIndexOptions(12)},
        //   2d
        {"spec": Gen2dIndex(8), "options": Gen2dIndexOptions(13)},
        //   Haystack
        {"spec": GenHaystackIndex(9), "options": GenHaystackIndexOptions(13)},

        // Text Indexes
        {"spec": GenTextIndex(10), "options": GenTextIndexOptions(14)},

        // Hashed Index
        {"spec": GenHashedIndex(10), "options": GenIndexOptions(14)},
    ];

    // Cursor interface
    var i = 0;
    return {
        "hasNext": function() {
            return i < testIndexes.length;
        },
        "next": function() {
            if (i >= testIndexes.length) {
                return undefined;
            }
            return testIndexes[i++];
        }
    };
}

//
// Generates a collection metadata object
//
// Interface:
//
// get() // Get a collection metadata object, based on the given options
//
// Options:
//
// {
//     "capped" : (true/false) // Return all capped collection metadata
// }
//
// Usage:
//
// var generator = new CollectionMetadataGenerator({ "capped" : true });
// var metadata = generator.get();
//
function CollectionMetadataGenerator(options) {
    var capped = true;
    var options = options || {};

    for (var option in options) {
        if (options.hasOwnProperty(option)) {
            if (option === 'capped') {
                if (typeof(options['capped']) !== 'boolean') {
                    throw Error(
                        "\"capped\" options must be boolean in CollectionMetadataGenerator");
                }
                capped = options['capped'];
            } else {
                throw Error("Unsupported key in options passed to CollectionMetadataGenerator: " +
                            option);
            }
        }
    }

    // Collection metadata we are using as a source for testing
    // db.createCollection(name, {capped: <Boolean>, autoIndexId: <Boolean>, size: <number>, max
    // <number>} )
    var cappedCollectionMetadata = {
        "capped": true,
        "size": 100000,
        "max": 2000,
        "usePowerOf2Sizes": true,
        //"autoIndexId" : false // XXX: this doesn't exist in 2.4
    };
    // We need to explicitly enable usePowerOf2Sizes, since it's the default in 2.6 but not in 2.4
    var normalCollectionMetadata = {"usePowerOf2Sizes": true};

    return {
        "get": function() {
            return capped ? cappedCollectionMetadata : normalCollectionMetadata;
        }
    };
}

//
// Wrapper for the above classes useful for passing into functions that expect a data generator
//
function CollectionDataGenerator(options) {
    return {
        "data": new DataGenerator(),
        "indexes": new IndexDataGenerator(),
        "collectionMetadata": new CollectionMetadataGenerator(options)
    };
}
