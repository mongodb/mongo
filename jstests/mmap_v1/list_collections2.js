// Test the listCollections command and system.namespaces

mydb = db.getSisterDB("list_collections1");
mydb.dropDatabase();

mydb.foo.insert({x: 5});

mydb.runCommand({create: "bar", temp: true});

res = mydb.runCommand("listCollections");
collections = new DBCommandCursor(db.getMongo(), res).toArray();

bar = collections.filter(function(x) {
    return x.name == "bar";
})[0];
foo = collections.filter(function(x) {
    return x.name == "foo";
})[0];

assert(bar);
assert(foo);

assert.eq(bar.name, mydb.bar.getName());
assert.eq(foo.name, mydb.foo.getName());

assert(mydb.bar.temp, tojson(bar));

getCollectionName = function(infoObj) {
    return infoObj.name;
};

assert.eq(mydb._getCollectionInfosSystemNamespaces().map(getCollectionName),
          mydb._getCollectionInfosCommand().map(getCollectionName));

assert.eq(mydb.getCollectionInfos().map(getCollectionName),
          mydb._getCollectionInfosCommand().map(getCollectionName));

// Test the listCollections command and querying system.namespaces when a filter is specified.
assert.eq(mydb._getCollectionInfosSystemNamespaces({name: "foo"}).map(getCollectionName),
          mydb._getCollectionInfosCommand({name: "foo"}).map(getCollectionName),
          "listCollections command and querying system.namespaces returned different results");

mydb.dropDatabase();
