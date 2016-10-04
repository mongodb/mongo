// Tests toString() in object constructor.
// Verifies that native functions do not expose the _native_function and _native_data properties.

var t = db.where5;

t.drop();

t.save({a: 1});

// Prints information on the document's _id field.
function printIdConstructor(doc) {
    // If doc is undefined, this function is running inside server.
    if (!doc) {
        doc = this;
    }

    // Verify that function and data fields are hidden.
    assert(!('_native_function' in sleep));
    assert(!('_native_data' in sleep));

    // Predicate for matching document in collection.
    return true;
}

print('Running JS function in server...');
assert.eq(t.find({$where: printIdConstructor}).itcount(), 1);

print('Running JS function in client...');
var doc = t.findOne();
printIdConstructor(doc);
