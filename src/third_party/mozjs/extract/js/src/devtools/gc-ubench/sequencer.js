/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// A Sequencer handles transitioning between different mutators. Typically, it
// will base the decision to transition on things like elapsed time, number of
// GCs observed, or similar. However, they might also implement a search for
// some result value by running for some time while measuring, tweaking
// parameters, and re-running until an in-range result is found.

var Sequencer = class {
  // Return the current mutator (of class AllocationLoad).
  get current() {
    throw new Error("unimplemented");
  }

  start(now = gHost.now()) {
    this.started = now;
  }

  // Called by user to handle advancing time. Subclasses will normally override
  // do_tick() instead. Returns the results of a trial if complete (the mutator
  // reached its allotted time or otherwise determined that its timing data
  // should be valid), and falsy otherwise.
  tick(now = gHost.now()) {
    if (this.done()) {
      throw new Error("tick() called on completed sequencer");
    }

    return this.do_tick(now);
  }

  // Implement in subclass to handle time advancing. Must return trial's result
  // if complete. Called by tick(), above.
  do_tick(now = gHost.now()) {
    throw new Error("unimplemented");
  }

  // Returns whether this sequencer is done running trials.
  done() {
    throw new Error("unimplemented");
  }

  restart(now = gHost.now()) {
    this.reset();
    this.start(now);
  }

  // Returns how long the current load has been running.
  currentLoadElapsed(now = gHost.now()) {
    return now - this.started;
  }
};

// Run a single trial of a mutator and be done.
var SingleMutatorSequencer = class extends Sequencer {
  constructor(mutator, perf, duration_sec) {
    super();
    this.mutator = mutator;
    this.perf = perf;
    if (!(duration_sec > 0)) {
      throw new Error(`invalid duration '${duration_sec}'`);
    }
    this.duration = duration_sec * 1000;
    this.state = 'init'; // init -> running -> done
    this.lastResult = undefined;
  }

  get current() {
    return this.state === 'done' ? undefined : this.mutator;
  }

  reset() {
    this.state = 'init';
  }

  start(now = gHost.now()) {
    if (this.state !== 'init') {
      throw new Error("cannot restart a single-mutator sequencer");
    }
    super.start(now);
    this.state = 'running';
    this.perf.on_load_start(this.current, now);
  }

  do_tick(now) {
    if (this.currentLoadElapsed(now) < this.duration) {
      return false;
    }

    const load = this.current;
    this.state = 'done';
    return this.perf.on_load_end(load, now);
  }

  done() {
    return this.state === 'done';
  }
};

// For each of series of sequencers, run until done.
var ChainSequencer = class extends Sequencer {
  constructor(sequencers) {
    super();
    this.sequencers = sequencers;
    this.idx = -1;
    this.state = sequencers.length ? 'init' : 'done'; // init -> running -> done
  }

  get current() {
    return this.idx >= 0 ? this.sequencers[this.idx].current : undefined;
  }

  reset() {
    this.state = 'init';
    this.idx = -1;
  }

  start(now = gHost.now()) {
    super.start(now);
    if (this.sequencers.length === 0) {
      this.state = 'done';
      return;
    }

    this.idx = 0;
    this.sequencers[0].start(now);
    this.state = 'running';
  }

  do_tick(now) {
    const sequencer = this.sequencers[this.idx];
    const trial_result = sequencer.do_tick(now);
    if (!trial_result) {
      return false; // Trial is still going.
    }

    if (!sequencer.done()) {
      // A single trial has completed, but the sequencer is not yet done.
      return trial_result;
    }

    this.idx++;
    if (this.idx < this.sequencers.length) {
      this.sequencers[this.idx].start();
    } else {
      this.idx = -1;
      this.state = 'done';
    }

    return trial_result;
  }

  done() {
    return this.state === 'done';
  }
};

var RunUntilSequencer = class extends Sequencer {
  constructor(sequencer, loadMgr) {
    super();
    this.loadMgr = loadMgr;
    this.sequencer = sequencer;

    // init -> running -> done
    this.state = sequencer.done() ? 'done' : 'init';
  }

  get current() {
    return this.sequencer?.current;
  }

  reset() {
    this.sequencer.reset();
    this.state = 'init';
  }

  start(now) {
    super.start(now);
    this.sequencer.start(now);
    this.initSearch(now);
    this.state = 'running';
  }

  initSearch(now) {}

  done() {
    return this.state === 'done';
  }

  do_tick(now) {
    const trial_result = this.sequencer.do_tick(now);
    if (trial_result) {
      if (this.searchComplete(trial_result)) {
        this.state = 'done';
      } else {
        this.sequencer.restart(now);
      }
    }
    return trial_result;
  }

  // Take the result of the last mutator run into account (only notified after
  // a mutator is complete, so cannot be used to decide when to end the
  // mutator.)
  searchComplete(result) {
    throw new Error("must implement in subclass");
  }
};

// Run trials, adjusting garbagePerFrame, until 50% of the frames are dropped.
var Find50Sequencer = class extends RunUntilSequencer {
  constructor(sequencer, loadMgr, goal=0.5, low_range=0.45, high_range=0.55) {
    super(sequencer, loadMgr);

    // Run trials with varying garbagePerFrame, looking for a setting that
    // drops 50% of the frames, until we have been searching in the range for
    // `persistence` times.
    this.low_range = low_range;
    this.goal = goal;
    this.high_range = high_range;
    this.persistence = 3;

    this.clear();
  }

  reset() {
    super.reset();
    this.clear();
  }

  clear() {
    this.garbagePerFrame = undefined;

    this.good = undefined;
    this.goodAt = undefined;
    this.bad = undefined;
    this.badAt = undefined;

    this.numInRange = 0;
  }

  start(now) {
    super.start(now);
    if (!this.done()) {
      this.garbagePerFrame = this.sequencer.current.garbagePerFrame;
    }
  }

  searchComplete(result) {
    print(
      `Saw ${percent(result.dropped_60fps_fraction)} with garbagePerFrame=${this.garbagePerFrame}`
    );

    // This is brittle with respect to noise. It might be better to do a linear
    // regression and stop at an error threshold.
    if (result.dropped_60fps_fraction < this.goal) {
      if (this.goodAt === undefined || this.goodAt < this.garbagePerFrame) {
        this.goodAt = this.garbagePerFrame;
        this.good = result.dropped_60fps_fraction;
      }
      if (this.badAt !== undefined) {
        this.garbagePerFrame = Math.trunc(
          (this.garbagePerFrame + this.badAt) / 2
        );
      } else {
        this.garbagePerFrame *= 2;
      }
    } else {
      if (this.badAt === undefined || this.badAt > this.garbagePerFrame) {
        this.badAt = this.garbagePerFrame;
        this.bad = result.dropped_60fps_fraction;
      }
      if (this.goodAt !== undefined) {
        this.garbagePerFrame = Math.trunc(
          (this.garbagePerFrame + this.goodAt) / 2
        );
      } else {
        this.garbagePerFrame = Math.trunc(this.garbagePerFrame / 2);
      }
    }

    if (
      this.low_range < result.dropped_60fps_fraction &&
      result.dropped_60fps_fraction < this.high_range
    ) {
      this.numInRange++;
      if (this.numInRange >= this.persistence) {
        return true;
      }
    }

    print(`next run with ${this.garbagePerFrame}`);
    this.loadMgr.change_garbagePerFrame(this.garbagePerFrame);

    return false;
  }
};
