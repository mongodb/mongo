/* SERVER-14237: The "size" parameter for non-capped collections
 * stopped being honored in HEAD
 */

var ns = "server_xxxxx";
db.getCollection(ns).drop();

/* Testing for a single constant size may accidentally coincide with
 * the default size of extents, resulting in a false-negative.
 */
var sizes = [      500 * 1024 + 1, // approx. 500 KiB
                  1024 * 1024 + 1, //           1 MiB
              2 * 1024 * 1024 + 1  //           2 MiB
            ];
for (var i = 0; i < sizes.length; i++) {
    db.createCollection(ns, { size: sizes[i] });

    var meta = db.system.namespaces.findOne({name: db + "." + ns});
    assert.eq(
        meta.options.size, sizes[i],
        "initial extent size recorded in system.namespaces");

    var stats = db.getCollection(ns).stats();
    assert.gte( // The size of extent will be quantized so "gte" not "eq".
        stats.storageSize, sizes[i],
        "initial extent size of non-capped collection");

    db.getCollection(ns).drop();
}
