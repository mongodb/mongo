// Fix for SERVER-32072
//
// Ensures round-trippability of int ids in DBRef's after a save/restore

const coll = db.dbref4;
coll.drop();

coll.insert({
    "refInt": DBRef("DBRef", NumberInt(1), "Ref"),
});

// we inserted something with an int
assert(coll.findOne({"refInt.$id": {$type: 16}}));

let doc = coll.findOne();
doc.x = 1;
coll.save(doc);

// after pulling it back and saving it again, still has an int
assert(coll.findOne({"refInt.$id": {$type: 16}}));
