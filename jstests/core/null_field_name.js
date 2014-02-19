// SERVER-10313: Test that null char in field name causes an error when converting to bson
assert.throws( function () { Object.bsonsize({"a\0":1}); },
               null,
               "null char in field name");

assert.throws( function () { Object.bsonsize({"\0asdf":1}); },
               null,
               "null char in field name");