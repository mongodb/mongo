"use strict";

var {runMerizoeBench} = (function() {

    /**
     * Spawns a merizoebench process with the specified options.
     *
     * If a plain JavaScript object is specified as the 'config' parameter, then it is serialized to
     * a file as a JSON string which is then specified as the config file for the merizoebench
     * process.
     */
    function runMerizoeBench(config, options = {}) {
        const args = ["merizoebench"];

        if (typeof config === "object") {
            const filename = MerizoRunner.dataPath + "merizoebench_config.json";
            writeFile(filename, tojson(config));
            args.push(filename);
        } else if (typeof config === "string") {
            args.push(config);
        } else {
            throw new Error("'config' parameter must be a string or an object");
        }

        if (!options.hasOwnProperty("dbpath")) {
            options.dbpath = MerizoRunner.dataDir;
        }

        for (let key of Object.keys(options)) {
            const value = options[key];
            if (value === null || value === undefined) {
                throw new Error(
                    "Value '" + value + "' for '" + key +
                    "' option is ambiguous; specify {flag: ''} to add --flag command line" +
                    " options'");
            }

            args.push("--" + key);
            if (value !== "") {
                args.push(value.toString());
            }
        }

        const exitCode = _runMerizoProgram(...args);
        assert.eq(0, exitCode, "encountered an error in merizoebench");
    }

    return {runMerizoeBench};
})();
