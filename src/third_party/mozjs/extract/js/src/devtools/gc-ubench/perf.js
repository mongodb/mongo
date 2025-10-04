/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Performance monitoring and calculation.

function round_up(val, interval) {
  return val + (interval - (val % interval));
}

// Class for inter-frame timing, which handles being paused and resumed.
var FrameTimer = class {
  constructor() {
    // Start time of the current active test, adjusted for any time spent
    // stopped (so `now - this.start` is how long the current active test
    // has run for.)
    this.start = undefined;

    // Timestamp of callback following the previous frame.
    this.prev = undefined;

    // Timestamp when drawing was paused, or zero if drawing is active.
    this.stopped = 0;
  }

  is_stopped() {
    return this.stopped != 0;
  }

  start_recording(now = gHost.now()) {
    this.start = this.prev = now;
  }

  on_frame_finished(now = gHost.now()) {
    const delay = now - this.prev;
    this.prev = now;
    return delay;
  }

  pause(now = gHost.now()) {
    this.stopped = now;
    // Abuse this.prev to store the time elapsed since the previous frame.
    // This will be used to adjust this.prev when we resume.
    this.prev = now - this.prev;
  }

  resume(now = gHost.now()) {
    this.prev = now - this.prev;
    const stop_duration = now - this.stopped;
    this.start += stop_duration;
    this.stopped = 0;
  }
};

// Per-frame time sampling infra.
var sampleTime = 16.666667; // ms
var sampleIndex = 0;

// Class for maintaining a rolling window of per-frame GC-related counters:
// inter-frame delay, minor/major/slice GC counts, cumulative bytes, etc.
var FrameHistory = class {
  constructor(numSamples) {
    // Private
    this._frameTimer = new FrameTimer();
    this._numSamples = numSamples;

    // Public API
    this.delays = new Array(numSamples);
    this.gcBytes = new Array(numSamples);
    this.mallocBytes = new Array(numSamples);
    this.gcs = new Array(numSamples);
    this.minorGCs = new Array(numSamples);
    this.majorGCs = new Array(numSamples);
    this.slices = new Array(numSamples);

    sampleIndex = 0;
    this.reset();
  }

  start(now = gHost.now()) {
    this._frameTimer.start_recording(now);
  }

  reset() {
    this.delays.fill(0);
    this.gcBytes.fill(0);
    this.mallocBytes.fill(0);
    this.gcs.fill(this.gcs[sampleIndex]);
    this.minorGCs.fill(this.minorGCs[sampleIndex]);
    this.majorGCs.fill(this.majorGCs[sampleIndex]);
    this.slices.fill(this.slices[sampleIndex]);

    sampleIndex = 0;
  }

  get numSamples() {
    return this._numSamples;
  }

  findMax(collection) {
    // Depends on having at least one non-negative entry, and unfilled
    // entries being <= max.
    var maxIndex = 0;
    for (let i = 0; i < this._numSamples; i++) {
      if (collection[i] >= collection[maxIndex]) {
        maxIndex = i;
      }
    }
    return maxIndex;
  }

  findMaxDelay() {
    return this.findMax(this.delays);
  }

  on_frame(now = gHost.now()) {
    const delay = this._frameTimer.on_frame_finished(now);

    // Total time elapsed while the active test has been running.
    var t = now - this._frameTimer.start;
    var newIndex = Math.round(t / sampleTime);
    while (sampleIndex < newIndex) {
      sampleIndex++;
      var idx = sampleIndex % this._numSamples;
      this.delays[idx] = delay;
      if (gHost.features.haveMemorySizes) {
        this.gcBytes[idx] = gHost.gcBytes;
        this.mallocBytes[idx] = gHost.mallocBytes;
      }
      if (gHost.features.haveGCCounts) {
        this.minorGCs[idx] = gHost.minorGCCount;
        this.majorGCs[idx] = gHost.majorGCCount;
        this.slices[idx] = gHost.GCSliceCount;
      }
    }

    return delay;
  }

  pause() {
    this._frameTimer.pause();
  }

  resume() {
    this._frameTimer.resume();
  }

  is_stopped() {
    return this._frameTimer.is_stopped();
  }
};

var PerfTracker = class {
  constructor() {
    // Private
    this._currentLoadStart = undefined;
    this._frameCount = undefined;
    this._mutating_ms = undefined;
    this._suspend_sec = undefined;
    this._minorGCs = undefined;
    this._majorGCs = undefined;

    // Public
    this.results = [];
  }

  on_load_start(load, now = gHost.now()) {
    this._currentLoadStart = now;
    this._frameCount = 0;
    this._mutating_ms = 0;
    this._suspend_sec = 0;
    this._majorGCs = gHost.majorGCCount;
    this._minorGCs = gHost.minorGCCount;
  }

  on_load_end(load, now = gHost.now()) {
    const elapsed_time = (now - this._currentLoadStart) / 1000;
    const full_time = round_up(elapsed_time, 1 / 60);
    const frame_60fps_limit = Math.round(full_time * 60);
    const dropped_60fps_frames = frame_60fps_limit - this._frameCount;
    const dropped_60fps_fraction = dropped_60fps_frames / frame_60fps_limit;

    const mutating_and_gc_fraction = this._mutating_ms / (full_time * 1000);

    const result = {
      load,
      elapsed_time,
      mutating: this._mutating_ms / 1000,
      mutating_and_gc_fraction,
      suspended: this._suspend_sec,
      full_time,
      frames: this._frameCount,
      dropped_60fps_frames,
      dropped_60fps_fraction,
      majorGCs: gHost.majorGCCount - this._majorGCs,
      minorGCs: gHost.minorGCCount - this._minorGCs,
    };
    this.results.push(result);

    this._currentLoadStart = undefined;
    this._frameCount = 0;

    return result;
  }

  after_suspend(wait_sec) {
    this._suspend_sec += wait_sec;
  }

  before_mutator(now = gHost.now()) {
    this._frameCount++;
  }

  after_mutator(start_time, end_time = gHost.now()) {
    this._mutating_ms += end_time - start_time;
  }
};
