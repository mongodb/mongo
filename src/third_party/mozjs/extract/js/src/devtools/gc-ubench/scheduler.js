/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Frame schedulers: executing a frame's worth of mutation, and possibly
// waiting for a later frame. (These schedulers will halt the main thread,
// allowing background threads to continue working.)

var Scheduler = class {
  constructor(perfMonitor) {
    this._perfMonitor = perfMonitor;
  }

  start(loadMgr, timestamp) {
    return loadMgr.start(timestamp);
  }
  tick(loadMgr, timestamp) {}
  wait_for_next_frame(t0, tick_start, tick_end) {}
};

// "Sync to vsync" scheduler: after the mutator is done for a frame, wait until
// the beginning of the next 60fps frame.
var VsyncScheduler = class extends Scheduler {
  tick(loadMgr, timestamp) {
    this._perfMonitor.before_mutator(timestamp);
    gHost.start_turn();
    const completed = loadMgr.tick(timestamp);
    gHost.end_turn();
    this._perfMonitor.after_mutator(timestamp);
    return completed;
  }

  wait_for_next_frame(t0, tick_start, tick_end) {
    // Compute how long until the next 60fps vsync event, and wait that long.
    const elapsed = (tick_end - t0) / 1000;
    const period = 1 / FPS;
    const used = elapsed % period;
    const delay = period - used;
    gHost.suspend(delay);
    this._perfMonitor.after_suspend(delay);
  }
};

// Try to maintain 60fps, but if we overrun a frame, do more processing
// immediately to make the next frame come up as soon as possible.
var OptimizeForFrameRate = class extends Scheduler {
  tick(loadMgr, timestamp) {
    this._perfMonitor.before_mutator(timestamp);
    gHost.start_turn();
    const completed = loadMgr.tick(timestamp);
    gHost.end_turn();
    this._perfMonitor.after_mutator(timestamp);
    return completed;
  }

  wait_for_next_frame(t0, tick_start, tick_end) {
    const next_frame_ms = round_up(tick_start, 1000 / FPS);
    if (tick_end < next_frame_ms) {
      const delay = (next_frame_ms - tick_end) / 1000;
      gHost.suspend(delay);
      this._perfMonitor.after_suspend(delay);
    }
  }
};
