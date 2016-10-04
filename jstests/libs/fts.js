// Utility functions for FTS tests
//
function queryIDS(coll, search, filter, extra, limit) {
    var query = {"$text": {"$search": search}};
    if (extra)
        query = {"$text": Object.extend({"$search": search}, extra)};
    if (filter)
        Object.extend(query, filter);

    var result;
    if (limit)
        result = coll.find(query, {score: {"$meta": "textScore"}})
                     .sort({score: {"$meta": "textScore"}})
                     .limit(limit);
    else
        result =
            coll.find(query, {score: {"$meta": "textScore"}}).sort({score: {"$meta": "textScore"}});

    return getIDS(result);
}

// Return an array of _ids from a cursor
function getIDS(cursor) {
    if (!cursor)
        return [];

    return cursor.map(function(z) {
        return z._id;
    });
}
