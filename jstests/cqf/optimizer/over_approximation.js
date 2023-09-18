/*
 * Tests that disjuntions that are over-approximated retain all predicates when computing the final
 * result. In this case, the disjunction of a conjunction ((X AND Y) OR Z), is over-approximated to
 * (X OR Z) during optimization, but Y should be reintroduced when matching documents.
 */

const t = db.over_approximation;
t.drop();

assert.commandWorked(t.insert({
    "_id": 0,
    "obj": {
        "_id": 1,
        "obj": {
            "_id": 2,
            "date": ISODate("2019-03-10T10:27:40.962Z"),
            "obj": {
                "_id": 3,
                "str": "parsing Chicken Handmade Concrete Pizza",
                "date": ISODate("2019-03-26T16:54:52.891Z")
            }
        }
    }
}))

const res =
    t.aggregate({
         $match: {
             // Key disjunction: combines an over-approximation interval with another interval.
             $or: [
                 {
                     // Together, these two are over-approximated as an interval: (minDate,
                     // 2019-11-22).
                     $and: [
                         // Can be converted into an interval (minDate, 2019-11-22).
                         {"obj.obj.obj.date": {$lt: new Date("2019-11-22T07:26:07.247Z")}},
                         {"obj.obj.obj.str": {$not: {$gte: "bottom-line contingency"}}}
                     ]
                 },
                 // Can be converted into an interval (2019-12-15, maxDate).
                 {"obj.obj.obj.date": {$gte: new Date("2019-12-15T15:20:50.078Z")}}
             ]
         }
     }).toArray();

assert.eq([], res, "No documents should match")
