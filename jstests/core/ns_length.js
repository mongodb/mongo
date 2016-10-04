// SERVER-7282 Faulty logic when testing maximum collection name length.

// constants from server
var maxNsLength = 127;
var maxNsCollectionLength = 120;

var myDb = db.getSiblingDB("ns_length");
myDb.dropDatabase();  // start empty

function mkStr(length) {
    s = "";
    while (s.length < length) {
        s += "x";
    }
    return s;
}

function canMakeCollectionWithName(name) {
    assert.eq(myDb.stats().storageSize, 0, "initial conditions");

    var success = false;
    try {
        // may either throw or return an error
        success = !(myDb[name].insert({}).hasWriteError());
    } catch (e) {
        success = false;
    }

    if (!success) {
        assert.eq(myDb.stats().storageSize, 0, "no files should be created on error");
        return false;
    }

    myDb.dropDatabase();
    return true;
}

function canMakeIndexWithName(collection, name) {
    var success = collection.ensureIndex({x: 1}, {name: name}).ok;
    if (success) {
        assert.commandWorked(collection.dropIndex(name));
    }
    return success;
}

function canRenameCollection(from, to) {
    var success = myDb[from].renameCollection(to).ok;
    if (success) {
        // put it back
        assert.commandWorked(myDb[to].renameCollection(from));
    }
    return success;
}

// test making collections around the name limit
var prefixOverhead = (myDb.getName() + ".").length;
var maxCollectionNameLength = maxNsCollectionLength - prefixOverhead;
for (var i = maxCollectionNameLength - 3; i <= maxCollectionNameLength + 3; i++) {
    assert.eq(canMakeCollectionWithName(mkStr(i)),
              i <= maxCollectionNameLength,
              "ns name length = " + (prefixOverhead + i));
}

// test making indexes around the name limit
var collection = myDb.collection;
collection.insert({});
var maxIndexNameLength = maxNsLength - (collection.getFullName() + ".$").length;
for (var i = maxIndexNameLength - 3; i <= maxIndexNameLength + 3; i++) {
    assert.eq(canMakeIndexWithName(collection, mkStr(i)),
              i <= maxIndexNameLength,
              "index ns name length = " + ((collection.getFullName() + ".$").length + i));
}

// test renaming collections with the destination around the name limit
myDb.from.insert({});
for (var i = maxCollectionNameLength - 3; i <= maxCollectionNameLength + 3; i++) {
    assert.eq(canRenameCollection("from", mkStr(i)),
              i <= maxCollectionNameLength,
              "new ns name length = " + (prefixOverhead + i));
}

// test renaming collections with the destination around the name limit due to long indexe names
myDb.from.ensureIndex({a: 1}, {name: mkStr(100)});
var indexNsNameOverhead = (myDb.getName() + "..$").length + 100;  // index ns name - collection name
var maxCollectionNameWithIndex = maxNsLength - indexNsNameOverhead;
for (var i = maxCollectionNameWithIndex - 3; i <= maxCollectionNameWithIndex + 3; i++) {
    assert.eq(canRenameCollection("from", mkStr(i)),
              i <= maxCollectionNameWithIndex,
              "index ns name length = " + (indexNsNameOverhead + i));
}
