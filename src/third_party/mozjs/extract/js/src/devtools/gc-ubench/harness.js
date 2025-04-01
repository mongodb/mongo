/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Global defaults

// Allocate this much "garbage" per frame. This might correspond exactly to a
// number of objects/values, or it might be some set of objects, depending on
// the mutator in question.
var gDefaultGarbagePerFrame = "8K";

// In order to avoid a performance cliff between when the per-frame garbage
// fits in the nursery and when it doesn't, most mutators will collect multiple
// "piles" of garbage and round-robin through them, so that the per-frame
// garbage stays alive for some number of frames. There will still be some
// internal temporary allocations that don't end up in the piles; presumably,
// the nursery will take care of those.
//
// If the per-frame garbage is K and the number of piles is P, then some of the
// garbage will start getting tenured as long as P*K > size(nursery).
var gDefaultGarbagePiles = "8";

var gDefaultTestDuration = 8.0;

// The Host interface that provides functionality needed by the test harnesses
// (web + various shells). Subclasses should override with the appropriate
// functionality. The methods that throw an error must be implemented. The ones
// that return undefined are optional.
//
// Note that currently the web UI doesn't really use the scheduling pieces of
// this.
var Host = class {
  constructor() {}
  start_turn() {
    throw new Error("unimplemented");
  }
  end_turn() {
    throw new Error("unimplemented");
  }
  suspend(duration) {
    throw new Error("unimplemented");
  } // Shell driver only
  now() {
    return performance.now();
  }

  minorGCCount() {
    return undefined;
  }
  majorGCCount() {
    return undefined;
  }
  GCSliceCount() {
    return undefined;
  }

  features = {
    haveMemorySizes: false,
    haveGCCounts: false,
  };
};

function percent(x) {
  return `${(x*100).toFixed(2)}%`;
}

function parse_units(v) {
  if (!v.length) {
    return NaN;
  }
  var lastChar = v[v.length - 1].toLowerCase();
  if (!isNaN(parseFloat(lastChar))) {
    return parseFloat(v);
  }
  var units = parseFloat(v.substr(0, v.length - 1));
  if (lastChar == "k") {
    return units * 1e3;
  }
  if (lastChar == "m") {
    return units * 1e6;
  }
  if (lastChar == "g") {
    return units * 1e9;
  }
  return NaN;
}

var AllocationLoad = class {
  constructor(info, name) {
    this.load = info;
    this.load.name = this.load.name ?? name;

    this._garbagePerFrame =
      info.garbagePerFrame ||
      parse_units(info.defaultGarbagePerFrame || gDefaultGarbagePerFrame);
    this._garbagePiles =
      info.garbagePiles ||
      parse_units(info.defaultGarbagePiles || gDefaultGarbagePiles);
  }

  get name() {
    return this.load.name;
  }
  get description() {
    return this.load.description;
  }
  get garbagePerFrame() {
    return this._garbagePerFrame;
  }
  set garbagePerFrame(amount) {
    this._garbagePerFrame = amount;
  }
  get garbagePiles() {
    return this._garbagePiles;
  }
  set garbagePiles(amount) {
    this._garbagePiles = amount;
  }

  start() {
    this.load.load(this._garbagePiles);
  }

  stop() {
    this.load.unload();
  }

  reload() {
    this.stop();
    this.start();
  }

  tick() {
    this.load.makeGarbage(this._garbagePerFrame);
  }

  is_dummy_load() {
    return this.load.name == "noAllocation";
  }
};

var AllocationLoadManager = class {
  constructor(tests) {
    this._loads = new Map();
    for (const [name, info] of tests.entries()) {
      this._loads.set(name, new AllocationLoad(info, name));
    }
    this._active = undefined;
    this._paused = false;

    // Public API
    this.sequencer = null;
    this.testDurationMS = gDefaultTestDuration * 1000;
  }

  getByName(name) {
    const mutator = this._loads.get(name);
    if (!mutator) {
      throw new Error(`invalid mutator '${name}'`);
    }
    return mutator;
  }

  activeLoad() {
    return this._active;
  }

  setActiveLoad(mutator) {
    if (this._active) {
      this._active.stop();
    }
    this._active = mutator;
    this._active.start();
  }

  deactivateLoad() {
    this._active.stop();
    this._active = undefined;
  }

  get paused() {
    return this._paused;
  }
  set paused(pause) {
    this._paused = pause;
  }

  load_running() {
    return this._active;
  }

  change_garbagePiles(amount) {
    if (this._active) {
      this._active.garbagePiles = amount;
      this._active.reload();
    }
  }

  change_garbagePerFrame(amount) {
    if (this._active) {
      this._active.garbagePerFrame = amount;
    }
  }

  tick(now = gHost.now()) {
    this.lastActive = this._active;
    let completed = false;

    if (this.sequencer) {
      if (this.sequencer.tick(now)) {
        completed = true;
        if (this.sequencer.current) {
          this.setActiveLoad(this.sequencer.current);
        } else {
          this.deactivateLoad();
        }
        if (this.sequencer.done()) {
          this.sequencer = null;
        }
      }
    }

    if (this._active && !this._paused) {
      this._active.tick();
    }

    return completed;
  }

  startSequencer(sequencer, now = gHost.now()) {
    this.sequencer = sequencer;
    this.sequencer.start(now);
    this.setActiveLoad(this.sequencer.current);
  }

  stopped() {
    return !this.sequencer || this.sequencer.done();
  }

  currentLoadRemaining(now = gHost.now()) {
    if (this.stopped()) {
      return 0;
    }

    // TODO: The web UI displays a countdown to the end of the current mutator.
    // This won't work for potential future things like "run until 3 major GCs
    // have been seen", so the API will need to be modified to provide
    // information in that case.
    return this.testDurationMS - this.sequencer.currentLoadElapsed(now);
  }
};

// Current test state.
var gLoadMgr = undefined;

function format_with_units(n, label, shortlabel, kbase) {
  function format(n, prefix, unit) {
    let s = Number.isInteger(n) ? n.toString() : n.toFixed(2);
    return `${s}${prefix}${unit}`;
  }

  if (n < kbase * 4) {
    return `${n} ${label}`;
  } else if (n < kbase ** 2 * 4) {
    return format(n / kbase, 'K', shortlabel);
  } else if (n < kbase ** 3 * 4) {
    return format(n / kbase ** 2, 'M', shortlabel);
  }
  return format(n / kbase ** 3, 'G', shortlabel);
}

function format_bytes(bytes) {
  return format_with_units(bytes, "bytes", "B", 1024);
}

function format_num(n) {
  return format_with_units(n, "", "", 1000);
}

function update_histogram(histogram, delay) {
  // Round to a whole number of 10us intervals to provide enough resolution to
  // capture a 16ms target with adequate accuracy.
  delay = Math.round(delay * 100) / 100;
  var current = histogram.has(delay) ? histogram.get(delay) : 0;
  histogram.set(delay, ++current);
}

// Compute a score based on the total ms we missed frames by per second.
function compute_test_score(histogram) {
  var score = 0;
  for (let [delay, count] of histogram) {
    score += Math.abs((delay - 1000 / 60) * count);
  }
  score = score / (gLoadMgr.testDurationMS / 1000);
  return Math.round(score * 1000) / 1000;
}

// Build a spark-lines histogram for the test results to show with the aggregate score.
function compute_spark_histogram_percents(histogram) {
  var ranges = [
    [-99999999, 16.6],
    [16.6, 16.8],
    [16.8, 25],
    [25, 33.4],
    [33.4, 60],
    [60, 100],
    [100, 300],
    [300, 99999999],
  ];
  var rescaled = new Map();
  for (let [delay] of histogram) {
    for (var i = 0; i < ranges.length; ++i) {
      var low = ranges[i][0];
      var high = ranges[i][1];
      if (low <= delay && delay < high) {
        update_histogram(rescaled, i);
        break;
      }
    }
  }
  var total = 0;
  for (const [, count] of rescaled) {
    total += count;
  }

  var spark = [];
  for (let i = 0; i < ranges.length; ++i) {
    const amt = rescaled.has(i) ? rescaled.get(i) : 0;
    spark.push(amt / total);
  }

  return spark;
}
