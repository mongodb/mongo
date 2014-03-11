(function() {
"use strict";

var documentUpgradeCheck = function(indexes, doc) {
    var goodSoFar = true;
    var invalidForStorage = Object.invalidForStorage(doc);
    if (invalidForStorage) {
        print("Document Error: document is no longer valid in 2.6 because " + invalidForStorage +
              ": "+ tojson(doc));
        goodSoFar = false;
    }
    indexes.forEach(function(idx) {
        if (keyTooLong({index: idx, doc: doc})) {
            print("Document Error: key for index '" + idx.name + "' (" + tojson(idx.key) + ")" +
                  " too long for document: " +tojson(doc));
            goodSoFar = false;
        }
    });
    return goodSoFar;
};

var indexUpgradeCheck = function(index) {
    var goodSoFar = true;
    var indexValid = validateIndexKey(index.key);
    if (!indexValid.ok) {
        print("Index Error: invalid index spec for index '" + index.name + "': " +
              tojson(index.key));
        goodSoFar = false;
    }
    return goodSoFar;
};

var collUpgradeCheck = function(collObj) {
    var fullName = collObj.getFullName();
    var collName = collObj.getName();
    var dbName = collObj.getDB().getName();
    print("\nChecking collection " + fullName);
    var dbObj = collObj.getDB();
    var goodSoFar = true;

    // check for _id index if and only if it should be present
    // no $, not oplog, not system
    var indexColl = dbObj.getSiblingDB(dbName).system.indexes;
    if (collName.indexOf('$') === -1 && 
        collName.indexOf("system.") !== 0 &&
        (dbName !== "local" || collName.indexOf("oplog.") !== 0)) {
        var idIdx = indexColl.find({ns: fullName, name:"_id_"}).addOption(DBQuery.Option.noTimeout);
        if (!idIdx.hasNext()) {
            print("Collection Error: lack of _id index on collection: " + fullName);
            goodSoFar = false;
        }
    }

    var indexes = [];
    // run index level checks on each index on the collection
    indexColl.find({ns: fullName}).addOption(DBQuery.Option.noTimeout).forEach( function(index) {
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

    var lastAlertTime = Date.now();
    var alertInterval = 10 * 1000; // 10 seconds
    var numDocs = 0;
    // run document level checks on each document in the collection
    var theColl = dbObj.getSiblingDB(dbName).getCollection(collName);
    theColl.find().addOption(DBQuery.Option.noTimeout).sort({$natural: 1}).forEach(
        function(doc) {
            numDocs++;

            if (!documentUpgradeCheck(indexes, doc)) {
                goodSoFar = false;
                lastAlertTime = Date.now();
            }
            var nowTime = Date.now();
            if (nowTime - lastAlertTime > alertInterval) {
                print(numDocs + " documents processed");
                lastAlertTime = nowTime;
            }
    });

    return goodSoFar;
};

var dbUpgradeCheck = function(dbObj) {
    print("\nChecking database " + dbObj.getName());
    var goodSoFar = true;

    // run collection level checks on each collection in the db
    dbObj.getCollectionNames().forEach(function(collName) {
        if (!collUpgradeCheck(dbObj.getCollection(collName))) {
            goodSoFar = false;
        }
    });

    return goodSoFar;
};

DB.prototype.upgradeCheck = function(obj) { 
    var self = this;
    // parse args if there are any
    if (obj) {
        // check collection if a collection is passed
        if (obj["collection"]) {
            // make sure a string was passed in for the collection
            if (typeof obj["collection"] !== "string") {
                throw Error("The collection field must contain a string");
            }
            else {
                print("Checking collection '" + self.getName() + '.' + obj["collection"] +
                      "' for 2.6 upgrade compatibility");
                if (collUpgradeCheck(self.getCollection(obj["collection"]))) {
                    print("Everything in '" + self.getName() + '.' + obj["collection"] +
                           "' is ready for the upgrade!");
                    return true;
                }
                print("To fix the problems above please consult " +
                       "http://dochub.mongodb.org/core/upgrade_checker_help");
                return false;
            }
        }
        else {
            throw Error("When providing an argument to upgradeCheck, it must be of the form " +
                        "{collection: <collectionNameString>}. Otherwise, it will check every " +
                        "collection in the database. If you would like to check all databases, " +
                        "run db.upgradeCheckAllDBs() from the admin database.");
        }
    }

    print("database '" + self.getName() + "' for 2.6 upgrade compatibility");
    if (dbUpgradeCheck(self, true)) {
        print("Everything in '" + self.getName() + "' is ready for the upgrade!");
        return true;
    }
    print("To fix the problems above please consult " +
          "http://dochub.mongodb.org/core/upgrade_checker_help");
    return false;
};

DB.prototype.upgradeCheckAllDBs = function() {
    var self = this;
    if (self.getName() !== "admin") {
        throw Error("db.upgradeCheckAllDBs() can only be run from the admin database");
    }

    var dbs = self.getMongo().getDBs();
    var goodSoFar = true;

    // run db level checks on each db
    dbs.databases.forEach(function(dbObj) {
        if (!dbUpgradeCheck(self.getSiblingDB(dbObj.name))) {
            goodSoFar = false;
        }
    });

    if (goodSoFar) {
        print("Everything is ready for the upgrade!");
        return true;
    }
    print("To fix the problems above please consult " +
          "http://dochub.mongodb.org/core/upgrade_checker_help");
    return false;
};

})();
