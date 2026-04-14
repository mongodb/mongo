/**
 * Allows the execution of the MongoDB Database Tools from within a jstest.
 *
 * The "fetch database tools" evergreen command makes those tools available
 * in the environment.
 *
 * @class
 */
export class Mongorestore {
    constructor() {
        this.uri = "mongodb://" + db.getMongo().host;
    }

    execute({
        archive,
        nsFrom = undefined,
        nsTo = undefined,
        drop = true,
        gzip = true,
        maintainInsertionOrder = true,
        numParallelCollections = 1,
        numInsertionWorkersPerCollection = 1,
    } = {}) {
        if (archive === undefined) {
            throw new Error("Archive must be provided to Mongorestore.execute()");
        }

        let args = [
            TestData.inEvergreen ? "../mongodb_database_tools/bin/mongorestore" : "mongorestore",
            "--uri",
            this.uri,
        ];

        if (nsFrom) {
            args.push(`--nsFrom=${nsFrom}`);
        }

        if (nsTo) {
            args.push(`--nsTo=${nsTo}`);
        }

        if (maintainInsertionOrder) {
            args.push("--maintainInsertionOrder");
        }

        if (numParallelCollections) {
            args.push(`--numParallelCollections=${numParallelCollections}`);
        }

        if (numInsertionWorkersPerCollection) {
            args.push(`--numInsertionWorkersPerCollection=${numInsertionWorkersPerCollection}`);
        }

        if (gzip) {
            args.push("--gzip");
        }

        if (drop) {
            args.push("--drop");
        }

        args.push(`--archive=${archive}`);

        assert.eq(runNonMongoProgram(...args), 0);
    }
}
