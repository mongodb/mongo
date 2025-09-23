/**
 * Helpers for generating and deleting extension .conf files in noPassthrough tests.
 */
import {getPython3Binary} from "jstests/libs/python.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * @param {string|string[]} soFileNames A shared object file name or a list of shared object file names
 *      to generate .conf files for.
 */
function _generateExtensionConfigsInternal(soFileNames, manualOptions = "") {
    if (!Array.isArray(soFileNames)) {
        soFileNames = [soFileNames];
    }

    const paths = soFileNames.map((name) => MongoRunner.getExtensionPath(name));

    // A hash is appended to the end of the configuration file name to avoid collisions
    // across concurrent tests using the same extension.
    const hash = UUID().hex();
    const args = [
        "-m",
        "buildscripts.resmokelib.extensions.generate_extension_configs",
        "--so-files",
        paths.join(","),
        "--with-suffix",
        hash,
    ];
    if (manualOptions) {
        args.push("--manual-options", manualOptions);
    }

    const ret = runNonMongoProgram(getPython3Binary(), ...args);
    assert.eq(ret, 0, "Failed to generate extension .conf files");

    return paths.map((p) => {
        // path/to/libfoo_mongo_extension.so -> foo_{hash}
        const fileName = p.substring(p.lastIndexOf("/") + 1);
        const extensionName = fileName.substring(0, fileName.lastIndexOf("."));
        return extensionName.replace(/^lib/, "").replace(/_mongo_extension$/, "") + `_${hash}`;
    });
}

/**
 * @param {string} path A path string to a .so file for which to generate a .conf file.
 * @param {string} manualOptions A YAML or JSON string containing extensionOptions to include in
 * each extension. This is used for noPassthrough tests where extension options are not already set
 * by configurations.yml.
 */
export function generateExtensionConfigWithOptions(path, manualOptions) {
    const extensionNames = _generateExtensionConfigsInternal([path], manualOptions);
    assert.eq(extensionNames.length, 1, "Expected exactly one extension config to be generated");
    return extensionNames[0];
}

/**
 * @param {string|string[]} paths A path string or a list of path strings to .so files for which to generate
 * default .conf files.
 */
export function generateExtensionConfigs(paths) {
    return _generateExtensionConfigsInternal(paths);
}

/**
 * @param {string[]} names A list of extension names to delete .conf files for.
 */
export function deleteExtensionConfigs(names) {
    const ret = runNonMongoProgram(
        getPython3Binary(),
        "-m",
        "buildscripts.resmokelib.extensions.delete_extension_configs",
        "--extension-names",
        names.join(","),
    );
    assert.eq(ret, 0, "Failed to delete extension .conf files");
}

/**
 * Runs a test function in environments with one or more extension loaded, ensuring proper
 * generation and cleanup of .conf files.
 */
export function withExtensions(extToOptionsMap, testFn) {
    const extensionsToLoad = [];

    for (const [extLib, extensionOptions] of Object.entries(extToOptionsMap)) {
        const cfgStr = typeof extensionOptions === "string" ? extensionOptions : JSON.stringify(extensionOptions);
        const extensionName = generateExtensionConfigWithOptions(extLib, cfgStr);
        extensionsToLoad.push(extensionName);
    }

    const options = {
        loadExtensions: extensionsToLoad,
    };

    try {
        {
            const mongodConn = MongoRunner.runMongod(options);
            testFn(mongodConn);
            MongoRunner.stopMongod(mongodConn);
        }

        {
            const shardingTest = new ShardingTest({
                shards: 1,
                mongos: 1,
                config: 1,
                mongosOptions: options,
            });
            testFn(shardingTest.s);
            shardingTest.stop();
        }
    } finally {
        deleteExtensionConfigs(extensionsToLoad);
    }
}

/**
 * Verifies that starting mongod or mongos with the given options fails as expected.
 */
export function checkExtensionFailsToLoad({options, st, validateExitCode = true}) {
    function tryStartup(startupFn) {
        try {
            startupFn();
            assert(false, "Expected startup to fail but it succeeded");
        } catch (e) {
            assert(e instanceof MongoRunner.StopError, e);
            if (validateExitCode) {
                assert.eq(e.returnCode, MongoRunner.EXIT_BADOPTIONS, e);
            }
        }
    }
    // Test mongod.
    tryStartup(() => {
        const conn = MongoRunner.runMongod(options);
        MongoRunner.stopMongod(conn);
    });
    // Test mongos.
    tryStartup(() => {
        const conn = MongoRunner.runMongos(Object.assign({configdb: st.configRS.getURL()}, options));
        MongoRunner.stopMongos(conn);
    });
}
