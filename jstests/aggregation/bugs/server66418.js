// SERVER-66418
// Bad projection created during dependency analysis due to string order assumption
(function() {
"use strict";

const coll = db[jsTest.name()];
coll.drop();

coll.save({
    _id: 1,
    type: 'PRODUCT',
    status: 'VALID',
    locale: {
        en: 'INSTRUMENT PANEL',
        es: 'INSTRUMENTOS DEL CUADRO',
        fr: 'INSTRUMENT TABLEAU DE BORD',
    }
});

// before SERVER-66418, this incorrectly threw a PathCollision error
coll.aggregate([
    {"$match": {"_id": 1}},
    {"$sort": {"_id": 1}},
    {
        "$project": {
            "designation": {
                "$switch": {
                    "branches": [{
                        "case": {"$eq": ["$type", "PRODUCT"]},
                        "then": {"$ifNull": ["$locale.en-GB.name", "$locale.en.name"]}
                    }],
                    "default": {"$ifNull": ["$locale.en-GB", "$locale.en"]}
                }
            }
        }
    }
]);
})();
