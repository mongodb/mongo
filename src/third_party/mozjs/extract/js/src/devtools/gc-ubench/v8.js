/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// V8 JS shell benchmark script
//
// Usage: run d8 v8.js -- --help

globalThis.loadRelativeToScript = load;

load("shell-bench.js");

var V8 = class extends Host {
  constructor() {
    super();
    this.waitTA = new Int32Array(new SharedArrayBuffer(4));
  }

  start_turn() {}

  end_turn() {}

  suspend(duration) {
    const response = Atomics.wait(this.waitTA, 0, 0, duration * 1000);
    if (response !== 'timed-out') {
      throw new Exception(`unexpected response from Atomics.wait: ${response}`);
    }
  }

  features = {
    haveMemorySizes: false,
    haveGCCounts: false
  };
};

var gHost = new V8();

var { opts, rest: mutators } = argparse.parse_args(arguments);
run(opts, mutators);

print("\nTest results:\n");
report_results();
