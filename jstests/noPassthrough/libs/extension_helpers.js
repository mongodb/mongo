/**
 * Helpers for generating and deleting extension .conf files in noPassthrough tests.
 */
import {getPython3Binary} from "jstests/libs/python.js";

/**
 * @param {string|string[]} soFileNames A shared object file name or a list of shared object file names
 *      to generate .conf files for.
 */
export function generateExtensionConfigs(soFileNames) {
    if (!Array.isArray(soFileNames)) {
        soFileNames = [soFileNames];
    }

    const paths = soFileNames.map((name) => MongoRunner.getExtensionPath(name));

    // A hash is appended to the end of the configuration file name to avoid collisions
    // across concurrent tests using the same extension.
    const hash = UUID().hex();
    const ret = runNonMongoProgram(
        getPython3Binary(),
        "-m",
        "buildscripts.resmokelib.extensions.generate_extension_configs",
        "--so-files",
        paths.join(","),
        "--with-suffix",
        hash,
    );
    assert.eq(ret, 0, "Failed to generate extension .conf files");

    let names = [];
    for (let i = 0; i < paths.length; i++) {
        // path/to/libfoo_mongo_extension.so -> foo_{hash}
        const fileName = paths[i].substring(paths[i].lastIndexOf("/") + 1);
        const extensionName = fileName.substring(0, fileName.lastIndexOf("."));

        names.push(extensionName.replace(/^lib/, "").replace(/_mongo_extension$/, "") + `_${hash}`);
    }

    return names;
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
