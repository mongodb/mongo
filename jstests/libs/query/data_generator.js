import {getPython3Binary} from "jstests/libs/python.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

/**
 * Allows the execution of the python data generator from
 * src/mongo/db/query/benchmark/data_generator from within a MongoDB jstest.
 *
 * For more information on the generator itself, see
 * src/mongo/db/query/benchmark/data_generator/README.md
 *
 * For an example of how to use this class, see
 * jstests/noPassthroughWithMongod/query/data_generator_integration.js
 * @class
 */
export class DataGenerator {
    static PROGRAM_PATH = "src/mongo/db/query/benchmark/data_generator/driver.py";
    constructor({db = null, module = null, seed = null} = {}) {
        if (db == null) {
            throw new Error("A db object must be provided to the DataGenerator constructor.");
        } else {
            this.dbName = db.getName();
            // The data_generator opens a connection to the database first and then
            // begins to generate data. So it will time out if the dataset is large.
            this.uri = "mongodb://" + db.getMongo().host + "/?socketTimeoutMS=1000000";
        }

        const tmpDir = _getEnv("TMPDIR") || _getEnv("TMP_DIR") || "/tmp";
        const randomUUID = extractUUIDFromObject(UUID());
        this.out = tmpDir + "/data_generator_" + randomUUID;

        if (module == null) {
            throw new Error("A data generator module name must be provided to DataGenerator constructor.");
        } else {
            this.module = module;
        }

        this.seed = seed;
    }

    execute({spec = null, size = null, indexes = null, drop = true, analyze = false, serial_inserts = true} = {}) {
        let args = [
            getPython3Binary(),
            DataGenerator.PROGRAM_PATH,
            "--uri",
            this.uri,
            "--db",
            this.dbName,
            "--out",
            this.out,
        ];

        if (spec == null || size == null) {
            throw new Error("Both specs and size must be provided to DataGenerator.execute()");
        }

        args.push("--size", size);

        if (indexes !== null) {
            args.push("--indexes", indexes);
        }

        if (this.seed !== null) {
            args.push("--seed", this.seed);
        }

        if (serial_inserts) {
            args.push("--serial-inserts");
        }

        if (drop) {
            args.push("--drop");
        }

        if (analyze) {
            args.push("--analyze");
        }

        args.push(this.module, spec);

        assert.eq(runNonMongoProgram(...args), 0);
    }

    cleanup() {
        removeFile(this.out);
    }
}
