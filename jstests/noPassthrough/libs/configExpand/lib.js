/**
 * Control the config file expansion mock web server.
 */
import {getPython3Binary} from "jstests/libs/python.js";

export class ConfigExpandRestServer {
    /**
     * Create a new webserver.
     */
    constructor() {
        this.python = getPython3Binary();
        print("Using python interpreter: " + this.python);

        this.web_server_py = "jstests/noPassthrough/libs/configExpand/rest_server.py";

        this.pid = undefined;
        this.port = undefined;
    }

    /**
     * Get the Port.
     *
     * @return {number} port number of http server
     */
    getPort() {
        return this.port;
    }

    /**
     * Get the URL.
     *
     * @return {string} url of http server
     */
    getURL() {
        return "http://localhost:" + this.port;
    }

    /**
     * Construct a string reflection URL.
     *
     * @param {string} string to be reflected
     * @param {object} Options, any combination of:
     * {
     *   sleep: int, // seconds to sleep during request
     * }
     */
    getStringReflectionURL(str, options = {}) {
        let url = this.getURL() + "/reflect/string?string=" + encodeURI(str);
        if (options.sleep !== undefined) {
            url += "&sleep=" + encodeURI(options.sleep);
        }
        return url;
    }

    /**
     *  Start the Mock HTTP Server.
     */
    start() {
        this.port = allocatePort();
        print("Mock Web server is listening on port: " + this.port);

        clearRawMongoProgramOutput();

        const args = [this.python, "-u", this.web_server_py, "--port=" + this.port];
        this.pid = _startMongoProgram({args: args});

        assert(checkProgram(this.pid));

        // Wait for the web server to start
        assert.soon(function () {
            return rawMongoProgramOutput(".*").search("Mock Web Server Listening") !== -1;
        });

        print("Mock HTTP Server sucessfully started.");
    }

    /**
     *  Stop the Mock HTTP Server.
     */
    stop() {
        stopMongoProgramByPid(this.pid);
    }
}

export function makeReflectionCmd(arg, opts = {}) {
    return function (arg, opts) {
        let cmd = getPython3Binary();
        cmd += " jstests/noPassthrough/libs/configExpand/reflect.py";

        if (opts.sleep && opts.sleep > 0) {
            cmd += " -s " + Number(opts.sleep);
        }

        // Escape arguments to the shell by wrapping in OS appropriate quotes.
        if (_isWindows()) {
            cmd +=
                " " +
                arg
                    .split('"')
                    .map((v) => '"' + v + '"')
                    .join('\\"');
        } else {
            cmd +=
                " " +
                arg
                    .split("'")
                    .map((v) => "'" + v + "'")
                    .join("\\'");
        }

        return cmd;
    }.call(this, arg, opts);
}

export function jsToYaml(config, toplevel = true) {
    if (typeof config === "object") {
        if (Array.isArray(config)) {
            let delim = "";
            let yaml = "";
            config.forEach(function (v) {
                yaml += delim + jsToYaml(v, false);
                delim = ",";
            });
            return "[" + yaml + "]";
        } else {
            let delim = "";
            let yaml = "";
            for (let k in config) {
                yaml += delim + k + ": " + jsToYaml(config[k], false);
                delim = toplevel ? "\n" : ",";
            }
            if (toplevel) {
                return yaml;
            } else {
                return "{" + yaml + "}";
            }
        }
    } else if (typeof config === "string") {
        return "'" + config.replace(/'/g, "''") + "'";
    } else {
        // Simple scalar JSON types are close enough to YAML types.
        return JSON.stringify(config);
    }
}

export function configExpandSuccess(config, test = null, opts = {}) {
    const configFile = MongoRunner.dataPath + "/configExpand.conf";
    writeFile(configFile, jsToYaml(config));

    let chmod = 0o600;
    if (opts.chmod !== undefined) {
        chmod = opts.chmod;
        delete opts.chmod;
    }

    if (!_isWindows()) {
        assert.eq(0, runNonMongoProgram("chmod", chmod.toString(8), configFile));
    }

    const mongod = MongoRunner.runMongod(
        Object.assign(
            {
                configExpand: "rest,exec",
                config: configFile,
            },
            opts,
        ),
    );

    assert(mongod, "Mongod failed to start up with config: " + cat(configFile));
    removeFile(configFile);

    if (test) {
        test(mongod.getDB("admin"));
    }
    MongoRunner.stopMongod(mongod);
}

export function configExpandFailure(config, test = null, opts = {}) {
    const configFile = MongoRunner.dataPath + "/configExpand.conf";
    writeFile(configFile, jsToYaml(config));

    let chmod = 0o600;
    if (opts.chmod !== undefined) {
        chmod = opts.chmod;
        delete opts.chmod;
    }

    if (!_isWindows()) {
        assert.eq(0, runNonMongoProgram("chmod", chmod.toString(8), configFile));
    }

    const options = Object.assign(
        {
            configExpand: "rest,exec",
            config: configFile,
            port: allocatePort(),
        },
        opts,
    );
    let args = [MongoRunner.getMongodPath()];
    for (let k in options) {
        args.push("--" + k);
        if (options[k] != "") {
            args.push(String(options[k]));
        }
    }

    clearRawMongoProgramOutput();
    const mongod = _startMongoProgram({args: args});

    assert.soon(function () {
        return rawMongoProgramOutput(".*").match(test);
    });
    if (mongod) {
        stopMongoProgramByPid(mongod);
    }
    removeFile(configFile);
}
