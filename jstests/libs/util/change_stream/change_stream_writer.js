/**
 * Writer class for executing commands in a background Thread.
 *
 * Commands are passed as BSON specs (from Command.toSpec()) and reconstructed
 * in the thread via Command.fromSpec(). Callers synchronize via Connector.waitForDone().
 */
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {Command} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {Thread} from "jstests/libs/parallelTester.js";

class Writer {
    static _threads = [];

    /**
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Writer instance name for Connector signaling.
     * @param {Array} commands - Command objects (converted to specs before crossing thread boundary).
     * @param {number} seed - Random seed for reproducibility.
     */
    static run(conn, instanceName, commands, seed) {
        const config = {
            instanceName,
            commandSpecs: commands.map((cmd) => cmd.toSpec()),
            seed,
        };
        const host = conn.host;
        const thread = new Thread(
            async function (host, config) {
                for (const override of TestData.threadOverrides || []) await import(override);
                const {Writer} = await import("jstests/libs/util/change_stream/change_stream_writer.js");
                const {Connector} = await import("jstests/libs/util/change_stream/change_stream_connector.js");
                const conn = new Mongo(host);
                try {
                    Writer._execute(conn, config);
                } catch (e) {
                    jsTest.log.info("Writer thread FAILED", {
                        instanceName: config.instanceName,
                        error: e.toString(),
                        stack: e.stack,
                    });
                    throw e;
                } finally {
                    Connector.notifyDone(conn, config.instanceName);
                }
            },
            host,
            config,
        );
        thread.start();
        Writer._threads.push(thread);
    }

    static joinAll() {
        const threads = Writer._threads;
        Writer._threads = [];
        const errors = [];
        for (const t of threads) {
            try {
                t.join();
            } catch (e) {
                errors.push(e);
            }
        }
        if (errors.length > 0) {
            jsTest.log.error("Writer threads failed", {errors});
            throw new Error(`${errors.length} Writer thread(s) failed: ${errors.map((e) => e.toString()).join("; ")}`);
        }
    }

    static _execute(conn, config) {
        Random.setRandomSeed(config.seed);

        for (let i = 0; i < config.commandSpecs.length; i++) {
            const spec = config.commandSpecs[i];
            const cmd = Command.fromSpec(spec);
            jsTest.log.debug(`Writer [${config.instanceName}]: cmd[${i}] ${spec.type}`);
            try {
                cmd.execute(conn);
            } catch (e) {
                jsTest.log.error(`Writer [${config.instanceName}]: cmd[${i}] ${spec.type} FAILED`, {
                    error: e.toString(),
                    spec: spec,
                });
                throw e;
            }
            Connector.heartbeat(conn, config.instanceName);
        }

        jsTest.log.info(`Writer [${config.instanceName}]: all ${config.commandSpecs.length} commands completed`);
    }
}

export {Writer};
