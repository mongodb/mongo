// Wrap whole file in a function to avoid polluting the global namespace
(function(){

startMongodTest = function (port, dirname, restart, extraOptions) {
    if (!port)
        port = MongoRunner.nextOpenPort();
    var f = startMongodEmpty;
    if (restart)
        f = startMongodNoReset;
    if (!dirname)
        dirname = "" + port; // e.g., data/db/27000

    var useHostname = false;
    if (extraOptions) {
         useHostname = extraOptions.useHostname;
         delete extraOptions.useHostname;
    }

    var options =
        {
            port: port,
            dbpath: MongoRunner.dataPath + dirname,
            noprealloc: "",
            smallfiles: "",
            oplogSize: "40",
            nohttpinterface: ""
        };

    if(jsTestOptions().noJournal)
        options["nojournal"] = "";
    if(jsTestOptions().noJournalPrealloc)
        options["nopreallocj"] = "";
    if(jsTestOptions().auth)
        options["auth"] = "";
    if(jsTestOptions().keyFile && (!extraOptions || !extraOptions['keyFile']))
        options['keyFile'] = jsTestOptions().keyFile

    if (extraOptions)
        Object.extend(options , extraOptions);

    var conn = f.apply(null,[options]);
    conn.name = (useHostname ? getHostName() : "localhost") + ":" + port;

    if (jsTestOptions().auth || jsTestOptions().keyFile || jsTestOptions().useX509)
        jsTest.authenticate(conn);

    return conn;
}

}());
