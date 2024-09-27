const originalMongo = globalThis.Mongo;

// Overrides new Mongo() to use gRPC when globally specified to do so.
// This behavior can be disabled or forced by specifying the "gRPC" option.
// e.g. new Mongo(uri, undefined, { gRPC: false });
globalThis.Mongo = function(uri, encryptedDBCallback, options) {
    // If the connection string specifies a grpc option, do not modify the call.
    if (uri.includes("grpc=")) {
        return originalMongo.call(this, uri, encryptedDBCallback, options);
    }

    let opts = options || {};
    if (opts.gRPC == undefined) {
        opts.gRPC = jsTestOptions().shellGRPC;
    }

    return originalMongo.call(this, uri, encryptedDBCallback, opts);
};

globalThis.Mongo.prototype = originalMongo.prototype;
