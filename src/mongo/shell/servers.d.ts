// type declarations for servers.js

declare module MongoRunner {
    function runMongod(opts?: object): Mongo
    function stopMongod(connection: Mongo): void
}

declare function myPort()
declare function runMongoProgram()
declare function startMongoProgram()
declare function startMongoProgramNoConnect()
