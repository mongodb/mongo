/**
 * FakeEmbeddingServer: starts a local Python HTTP server that speaks the
 * OpenAI-compatible embedding API and exposes admin endpoints for controlling
 * test behavior.
 *
 * Usage:
 *   const fake = new FakeEmbeddingServer();
 *   fake.start();
 *   // ...tests...
 *   fake.stop();
 */

import {getPython3Binary} from "jstests/libs/python.js";

const _SCRIPT =
    "src/mongo/db/modules/enterprise/jstests/streams/fake_embedding_server.py";

function _tmpPath(tag) {
    return "/tmp/fake_embed_" + tag + "_" + Math.floor(Math.random() * 1e9) + ".json";
}

function _adminPost(url, body) {
    const tmp = _tmpPath("post");
    const code = runNonMongoProgram(
        "curl", "-s", "--max-time", "5", "-o", tmp, "-X", "POST",
        "-H", "Content-Type: application/json",
        "-d", JSON.stringify(body),
        url);
    if (code !== 0) {
        throw new Error("curl POST to " + url + " failed with code " + code);
    }
    const resp = JSON.parse(cat(tmp));
    removeFile(tmp);
    return resp;
}

function _adminGet(url) {
    const tmp = _tmpPath("get");
    const code = runNonMongoProgram("curl", "-s", "--max-time", "5", "-o", tmp, url);
    if (code !== 0) {
        throw new Error("curl GET " + url + " failed with code " + code);
    }
    const resp = JSON.parse(cat(tmp));
    removeFile(tmp);
    return resp;
}

export class FakeEmbeddingServer {
    constructor() {
        this._port = undefined;
        this._pid = undefined;
    }

    start() {
        this._port = allocatePort();
        const python = getPython3Binary();
        clearRawMongoProgramOutput();
        this._pid = _startMongoProgram({
            args: [python, "-u", _SCRIPT, "--port=" + this._port],
        });
        assert(checkProgram(this._pid), "fake embedding server process died immediately");
        assert.soon(
            () => rawMongoProgramOutput(".*").includes("Fake Embedding Server listening"),
            "fake embedding server did not start");
    }

    stop() {
        if (this._pid !== undefined) {
            stopMongoProgramByPid(this._pid);
            this._pid = undefined;
        }
    }

    url() {
        return "http://localhost:" + this._port;
    }

    reset() {
        _adminPost(this.url() + "/admin/reset", {});
    }

    setMode(mode) {
        _adminPost(this.url() + "/admin/mode", {mode});
    }

    callCount() {
        return _adminGet(this.url() + "/admin/callcount").callCount;
    }

    capturedInputs() {
        return _adminGet(this.url() + "/admin/inputs").inputs;
    }

    /**
     * Pre-load a sequence of HTTP responses the server will return in order.
     * Each entry: {code: <HTTP status>, body: <JSON string or null for echo>}.
     * Once the sequence is exhausted the server falls back to echo mode.
     */
    setResponseSequence(responses) {
        _adminPost(this.url() + "/admin/sequence", {responses});
    }
}
