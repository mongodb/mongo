// Testing documents that contain empty field names. This was written as part of
// SERVER-86619.
db.emptyFields.drop();
db.createCollection('emptyFields');

const kNumDocs = 50;

for (let i = 0; i < 50; ++i) {
    db.emptyFields.insert({"": 123, "b": 456, sortField: i});
}

assert.eq(db.emptyFields
              .aggregate([
                  {$sort: {sortField: 1}},
                  {$addFields: {"m": {$meta: "sortKey"}}},
                  {$match: {"b": 456}}
              ])
              .itcount(),
          kNumDocs);
