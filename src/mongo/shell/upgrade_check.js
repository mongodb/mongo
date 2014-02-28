documentUpgradeCheck = function(indexes, doc) {
    var goodSoFar = true;
    var invalidForStorage = Object.invalidForStorage(doc);
    if (invalidForStorage) {
        print("Document:\n\t" + tojson(doc) + "\n\tis no longer valid in 2.6: " + invalidForStorage);
        goodSoFar = false;
    }
    indexes.forEach(function(idx) {
        if (keyTooLong({index: idx, doc: doc})) {
            print("Document:\n\t" + tojson(doc) + "\n\tcannot be indexed by: " + idx.name);
            if (idx.name === "_id_") {
                print("As a result, this document cannot be removed by its _id index. Instead, " +
                      "try to remove it by all other fields (omit any fields that have been " +
                      "identified as unindexable by the upgrade checker).");
            }
            else {
                print("Remove this document via the _id field");
            }
            goodSoFar = false;
        }
    });
    return goodSoFar;
}

indexUpgradeCheck = function(index) {
    var goodSoFar = true;
    var indexValid = validateIndexKey(index.key);
    if (!indexValid.ok) {
        print("Index: " + index.name + " is invalid: " + index.key);
        goodSoFar = false;
    }
    return goodSoFar;
}

collUpgradeCheck = function(dbName, collName) {
    var fullName = dbName + '.' + collName;
    print("\tChecking collection " + fullName);
    var goodSoFar = true;

    // check for _id index if and only if it should be present
    // no $, not oplog, not system
    if (collName.indexOf('$') === -1 && 
        collName.indexOf("system.") !== 0 &&
        (dbName !== "local" || collName.indexOf("oplog.") !== 0)) {
        var idIdx = db.getSiblingDB(dbName).system.indexes.find({ns: fullName, name: "_id_"});
        if (!idIdx.hasNext()) {
            print(dbName + '.' + collName +
                  " lacks an _id index; to fix this please run the following in the MongoDB shell");
            print("db.getSiblingDB('" + dbName + "')." + collName +
                  ".ensureIndex({_id: 1}, {unique: true});");
            goodSoFar = false;
        }
    }

    var indexes = [];
    // run index level checks on each index on the collection
    db.getSiblingDB(dbName).system.indexes.find({ns: fullName}).forEach(function(index) {
        if (!indexUpgradeCheck(index)) {
            goodSoFar = false;
        }
        else {
            // add its key to the list of index keys to check documents against
            indexes.push(index);
        }
    });

    // do not validate the documents in config dbs, oplog, or system collections
    if (collName.indexOf("system.") === 0 ||
        dbName === "config" ||
        (dbName === "local" && collName.indexOf("oplog.") === 0)) {
        return goodSoFar;
    }

    // run document level checks on each document in the collection
    db.getSiblingDB(dbName).getCollection(collName).find().sort({$natural: 1}).forEach(
        function(doc) {
            if (!documentUpgradeCheck(indexes, doc)) {
                goodSoFar = false;
            }
    });

    return goodSoFar;
}

dbUpgradeCheck = function(dbName) {
    print("\tChecking database " + dbName);
    var goodSoFar = true;

    // run collection level checks on each collection in the db
    db.getSiblingDB(dbName).getCollectionNames().forEach(function(collName) {
        if (!collUpgradeCheck(dbName, collName)) {
            goodSoFar = false;
        }
    });

    return goodSoFar;
}

upgradeCheck = function(obj) { 
    // parse args if there are any
    if (obj) {
        // check collection if a collection is passed
        if (obj["collection"]) {
            // make sure a string was passed in for the collection
            if (typeof obj["collection"] !== "string") {
                print("The collection field must contain a string");
                return false;
            }
        }

        if (obj["db"]) {
            // make sure a string was passed in for the db
            if (typeof obj["db"] !== "string") {
                print("The db field must contain a string");
                return false;
            }

            // check only the collection if it was passed it
            if (obj["collection"]) {
                print("\tChecking collection '" + obj["db"] + '.' + obj["collection"] +
                      "' for 2.6 upgrade compatibility");
                return collUpgradeCheck(obj["db"], obj["collection"]);
            }

            //otherwise check entire db
            print("\tChecking database '" + obj["db"] + "' for 2.6 upgrade compatibility");
            return dbUpgradeCheck(obj["db"]);
        }
        if (obj["collection"]) {
            print("\tChecking collection '" + db.getName() + '.' + obj["collection"] +
                  "' for 2.6 upgrade compatibility");
            return collUpgradeCheck(db.getName(), obj["collection"]);
        }
        print("When passing an argument to upgradeCheck, it must be of the form {db: " +
              "<dbNameString>, collection: <collectionNameString>} (either field may be omitted)");
        return false;
    }

    print("\tChecking mongod instance for 2.6 upgrade compatibility");
    var dbs = db.getMongo().getDBs();
    var goodSoFar = true;

    // run db level checks on each db
    dbs.databases.forEach(function(dbObj) {
        if (!dbUpgradeCheck(dbObj.name)) {
            goodSoFar = false;
        }
    });

    if (goodSoFar) {
        return "Everything is ready for the upgrade!";
    }
    return "The above comments explain the problems that should be fixed prior to upgrading";
}
