function storageEngineIsWiredTigerOrInMemory() {
    // We assume that WiredTiger is the default storage engine, if the storage engine is
    // unspecified in the test options.
    return !jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger" ||
        jsTest.options().storageEngine === "inMemory";
}

function storageEngineIsWiredTiger() {
    // We assume that WiredTiger is the default storage engine, if the storage engine is
    // unspecified in the test options.
    return !jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger";
}
