// SERVER-848 and SERVER-1191.
db.places.drop()

n = 0;
db.places.save({ "_id": n++, "loc" : { "x" : 4.9999, "y" : 52 } })
db.places.save({ "_id": n++, "loc" : { "x" : 5, "y" : 52 } })
db.places.save({ "_id": n++, "loc" : { "x" : 5.0001, "y" : 52 } })
db.places.save({ "_id": n++, "loc" : { "x" : 5, "y" : 52.0001 } })
db.places.save({ "_id": n++, "loc" : { "x" : 5, "y" : 51.9999 } })
db.places.save({ "_id": n++, "loc" : { "x" : 4.9999, "y" : 52.0001 } })
db.places.save({ "_id": n++, "loc" : { "x" : 5.0001, "y" : 52.0001 } })
db.places.save({ "_id": n++, "loc" : { "x" : 4.9999, "y" : 51.9999 } })
db.places.save({ "_id": n++, "loc" : { "x" : 5.0001, "y" : 51.9999 } })
db.places.ensureIndex( { loc : "2d" } )
radius=0.0001
center=[5,52]
//print(db.places.find({"loc" : {"$within" : {"$center" : [center, radius]}}}).count())
// FIXME: we want an assert, e.g., that there be 5 answers in the find().
db.places.find({"loc" : {"$within" : {"$center" : [center, radius]}}}).forEach(printjson);


// the result:
// { "_id" : ObjectId("4bb1f2f088df513435bcb4e1"), "loc" : { "x" : 5, "y" : 52 } }
// { "_id" : ObjectId("4bb1f54383459c40223a8ae7"), "loc" : { "x" : 5, "y" : 51.9999 } }
// { "_id" : ObjectId("4bb1f54583459c40223a8aeb"), "loc" : { "x" : 5.0001, "y" : 51.9999 } }
// { "_id" : ObjectId("4bb1f2e588df513435bcb4e0"), "loc" : { "x" : 4.9999, "y" : 52 } }
// { "_id" : ObjectId("4bb1f30888df513435bcb4e2"), "loc" : { "x" : 5.0001, "y" : 52 } }
// { "_id" : ObjectId("4bb1f54383459c40223a8ae8"), "loc" : { "x" : 4.9999, "y" : 52.0001 } }
