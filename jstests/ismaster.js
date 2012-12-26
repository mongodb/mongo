var
var res = db.isMaster();
assert( res.maxBsonObjectSize &&
        isNumber(res.maxBsonObjectSize) &&
        res.maxBsonObjectSize > 0, "maxBsonObjectSize possibly missing:" + tojson(res))
assert( res.maxMessageSizeBytes &&
        isNumber(res.maxMessageSizeBytes) &&
        res.maxBsonObjectSize > 0, "maxMessageSizeBytes possibly missing:" + tojson(res))
assert(res.ismaster, "ismaster missing or false:" + tojson(res))
assert(res.localTime, "localTime possibly missing:" + tojson(res))