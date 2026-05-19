"""
Fake embedding HTTP server for streams $embed integration tests.

Listens on an arbitrary port and responds to POST /v1/embeddings with an
OpenAI-compatible response shape: {"data": [{"embedding": [...]}, ...]}.

Server behavior is controlled through admin endpoints:
  POST /admin/mode       {"mode": "echo" | "captureInputs"}
  POST /admin/sequence   {"responses": [{"code": N, "body": "..."},...]}
  GET  /admin/callcount  -> {"callCount": N}
  GET  /admin/inputs     -> {"inputs": ["...", ...]}
  POST /admin/reset      resets all state to defaults

Echo mode (default): each input string produces a 3-element embedding whose
first component is the string length (deterministic, so identical inputs yield
identical vectors).

CaptureInputs mode: same echoed embeddings, but all input strings are recorded
for later inspection via /admin/inputs.

Sequence mode: responses come from a pre-loaded list. When the list is
exhausted the server falls back to echo mode.
"""

import argparse
import http.server
import json
import threading


class FakeEmbeddingHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress default access logging

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length)

    def _send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, code, text):
        body = text.encode() if isinstance(text, str) else text
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    @staticmethod
    def _echo_embeddings(inputs):
        data = []
        for i, text in enumerate(inputs):
            n = len(text)
            data.append({"embedding": [float(n), float(n) * 0.5, float(n) * 0.25], "index": i})
        return {"data": data}

    def do_POST(self):
        state = self.server.state
        raw = self._read_body()

        if self.path == "/admin/mode":
            cfg = json.loads(raw)
            with state["lock"]:
                state["mode"] = cfg["mode"]
            self._send_json(200, {"ok": True})
            return

        if self.path == "/admin/sequence":
            cfg = json.loads(raw)
            with state["lock"]:
                state["sequence"] = list(cfg["responses"])
            self._send_json(200, {"ok": True})
            return

        if self.path == "/admin/reset":
            with state["lock"]:
                state["callCount"] = 0
                state["inputs"] = []
                state["mode"] = "echo"
                state["sequence"] = []
            self._send_json(200, {"ok": True})
            return

        if self.path.startswith("/v1/embeddings"):
            try:
                payload = json.loads(raw)
                inputs = payload.get("input", [])
            except Exception:
                inputs = []

            with state["lock"]:
                state["callCount"] += 1
                # Consume next queued response if available.
                if state["sequence"]:
                    resp = state["sequence"].pop(0)
                    if resp["body"] is None:
                        # null body means "echo the inputs normally"
                        self._send_json(resp["code"], self._echo_embeddings(inputs))
                    else:
                        body_bytes = resp["body"].encode()
                        self.send_response(resp["code"])
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Content-Length", len(body_bytes))
                        self.end_headers()
                        self.wfile.write(body_bytes)
                    return

                mode = state["mode"]
                if mode == "captureInputs":
                    state["inputs"].extend(inputs)

            self._send_json(200, self._echo_embeddings(inputs))
            return

        self._send_text(404, "not found")

    def do_GET(self):
        state = self.server.state

        if self.path == "/admin/callcount":
            with state["lock"]:
                count = state["callCount"]
            self._send_json(200, {"callCount": count})
            return

        if self.path == "/admin/inputs":
            with state["lock"]:
                inputs = list(state["inputs"])
            self._send_json(200, {"inputs": inputs})
            return

        self._send_text(404, "not found")


def run(port):
    server = http.server.HTTPServer(("", port), FakeEmbeddingHandler)
    server.state = {
        "lock": threading.Lock(),
        "callCount": 0,
        "inputs": [],
        "mode": "echo",
        "sequence": [],
    }
    print(f"Fake Embedding Server listening on port {port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    run(args.port)
