/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

var stroke = {
  gcslice: "rgb(255,100,0)",
  minor: "rgb(0,255,100)",
  initialMajor: "rgb(180,60,255)",
};

var numSamples = 500;

var tests = new Map();

var gHistogram = new Map(); // {ms: count}
var gHistory = new FrameHistory(numSamples);
var gPerf = new PerfTracker();

var latencyGraph;
var memoryGraph;
var ctx;
var memoryCtx;

var loadState = "(init)"; // One of '(active)', '(inactive)', '(N/A)'
var testState = "idle"; // One of 'idle' or 'running'.
var enabled = { trackingSizes: false };

var gMemory = performance.mozMemory?.gc || performance.mozMemory || {};

var Firefox = class extends Host {
  start_turn() {
    // Handled by Gecko.
  }

  end_turn() {
    // Handled by Gecko.
  }

  suspend(duration) {
    // Not used; requestAnimationFrame takes its place.
    throw new Error("unimplemented");
  }

  get minorGCCount() {
    return gMemory.minorGCCount;
  }
  get majorGCCount() {
    return gMemory.majorGCCount;
  }
  get GCSliceCount() {
    return gMemory.sliceCount;
  }
  get gcBytes() {
    return gMemory.zone.gcBytes;
  }
  get mallocBytes() {
    return gMemory.zone.mallocBytes;
  }
  get gcAllocTrigger() {
    return gMemory.zone.gcAllocTrigger;
  }
  get mallocTrigger() {
    return gMemory.zone.mallocTriggerBytes;
  }

  features = {
    haveMemorySizes: 'gcBytes' in gMemory,
    haveGCCounts: 'majorGCCount' in gMemory,
  };
};

var gHost = new Firefox();

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

var Graph = class {
  constructor(canvas) {
    this.ctx = canvas.getContext('2d');

    // Adjust scale for high-DPI displays.
    this.scale = window.devicePixelRatio || 1;
    let rect = canvas.getBoundingClientRect();
    canvas.width = Math.floor(rect.width * this.scale);
    canvas.height = Math.floor(rect.height * this.scale);
    canvas.style.width = rect.width;
    canvas.style.height = rect.height;

    // Record canvas size to draw into.
    this.width = canvas.width;
    this.height = canvas.height;

    this.layout = {
      xAxisLabel_Y: this.height - 20 * this.scale,
    };
  }

  xpos(index) {
    return (index / numSamples) * (this.width - 100 * this.scale);
  }

  clear() {
    this.ctx.clearRect(0, 0, this.width, this.height);
  }

  drawScale(delay) {
    this.drawHBar(delay, `${delay}ms`, "rgb(150,150,150)");
  }

  draw60fps() {
    this.drawHBar(1000 / 60, "60fps", "#00cf61", 25);
  }

  draw30fps() {
    this.drawHBar(1000 / 30, "30fps", "#cf0061", 25);
  }

  drawAxisLabels(x_label, y_label) {
    const ctx = this.ctx;

    ctx.font = `${10 * this.scale}px sans-serif`;

    ctx.fillText(x_label, this.width / 2, this.layout.xAxisLabel_Y);

    ctx.save();
    ctx.rotate(Math.PI / 2);
    var start = this.height / 2 - ctx.measureText(y_label).width / 2;
    ctx.fillText(y_label, start, -this.width + 20 * this.scale);
    ctx.restore();
  }

  drawFrame() {
    const ctx = this.ctx;
    const width = this.width;
    const height = this.height;

    // Draw frame to show size
    ctx.strokeStyle = "rgb(0,0,0)";
    ctx.fillStyle = "rgb(0,0,0)";
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(width, 0);
    ctx.lineTo(width, height);
    ctx.lineTo(0, height);
    ctx.closePath();
    ctx.stroke();
  }
};

var LatencyGraph = class extends Graph {
  constructor(ctx) {
    super(ctx);
  }

  ypos(delay) {
    return this.height + this.scale * (100 - Math.log(delay) * 64);
  }

  drawHBar(delay, label, color = "rgb(0,0,0)", label_offset = 0) {
    const ctx = this.ctx;

    let y = this.ypos(delay);

    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    ctx.fillText(
      label,
      this.xpos(numSamples) + 4 + label_offset,
      this.ypos(delay) + 3
    );

    ctx.beginPath();
    ctx.moveTo(this.xpos(0), this.ypos(delay));
    ctx.lineTo(this.xpos(numSamples) + label_offset, this.ypos(delay));
    ctx.stroke();
    ctx.strokeStyle = "rgb(0,0,0)";
    ctx.fillStyle = "rgb(0,0,0)";
  }

  draw() {
    const ctx = this.ctx;

    this.clear();
    this.drawFrame();

    for (var delay of [10, 20, 30, 50, 100, 200, 400, 800]) {
      this.drawScale(delay);
    }
    this.draw60fps();
    this.draw30fps();

    var worst = 0,
      worstpos = 0;
    ctx.beginPath();
    for (let i = 0; i < numSamples; i++) {
      ctx.lineTo(this.xpos(i), this.ypos(gHistory.delays[i]));
      if (gHistory.delays[i] >= worst) {
        worst = gHistory.delays[i];
        worstpos = i;
      }
    }
    ctx.stroke();

    // Draw vertical lines marking minor and major GCs
    if (gHost.features.haveGCCounts) {
      ctx.strokeStyle = stroke.gcslice;
      let idx = sampleIndex % numSamples;
      const count = {
        major: gHistory.majorGCs[idx],
        minor: 0,
        slice: gHistory.slices[idx],
      };
      for (let i = 0; i < numSamples; i++) {
        idx = (sampleIndex + i) % numSamples;
        const isMajorStart = count.major < gHistory.majorGCs[idx];
        if (count.slice < gHistory.slices[idx]) {
          if (isMajorStart) {
            ctx.strokeStyle = stroke.initialMajor;
          }
          ctx.beginPath();
          ctx.moveTo(this.xpos(idx), 0);
          ctx.lineTo(this.xpos(idx), this.layout.xAxisLabel_Y);
          ctx.stroke();
          if (isMajorStart) {
            ctx.strokeStyle = stroke.gcslice;
          }
        }
        count.major = gHistory.majorGCs[idx];
        count.slice = gHistory.slices[idx];
      }

      ctx.strokeStyle = stroke.minor;
      idx = sampleIndex % numSamples;
      count.minor = gHistory.minorGCs[idx];
      for (let i = 0; i < numSamples; i++) {
        idx = (sampleIndex + i) % numSamples;
        if (count.minor < gHistory.minorGCs[idx]) {
          ctx.beginPath();
          ctx.moveTo(this.xpos(idx), 0);
          ctx.lineTo(this.xpos(idx), 20);
          ctx.stroke();
        }
        count.minor = gHistory.minorGCs[idx];
      }
    }

    ctx.fillStyle = "rgb(255,0,0)";
    if (worst) {
      ctx.fillText(
        `${worst.toFixed(2)}ms`,
        this.xpos(worstpos) - 10,
        this.ypos(worst) - 14
      );
    }

    // Mark and label the slowest frame
    ctx.beginPath();
    var where = sampleIndex % numSamples;
    ctx.arc(
      this.xpos(where),
      this.ypos(gHistory.delays[where]),
      5,
      0,
      Math.PI * 2,
      true
    );
    ctx.fill();
    ctx.fillStyle = "rgb(0,0,0)";

    this.drawAxisLabels("Time", "Pause between frames (log scale)");
  }
};

var MemoryGraph = class extends Graph {
  constructor(ctx) {
    super(ctx);
    this.range = 1;
  }

  ypos(size) {
    const percent = size / this.range;
    return (1 - percent) * this.height * 0.9 + this.scale * 20;
  }

  drawHBarForBytes(size, name, color) {
    this.drawHBar(size, `${format_bytes(size)} ${name}`, color)
  }

  drawHBar(size, label, color) {
    const ctx = this.ctx;

    const y = this.ypos(size);

    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    ctx.fillText(label, this.xpos(numSamples) + 4, y + 3);

    ctx.beginPath();
    ctx.moveTo(this.xpos(0), y);
    ctx.lineTo(this.xpos(numSamples), y);
    ctx.stroke();
    ctx.strokeStyle = "rgb(0,0,0)";
    ctx.fillStyle = "rgb(0,0,0)";
  }

  draw() {
    const ctx = this.ctx;

    this.clear();
    this.drawFrame();

    let gcMaxPos = 0;
    let mallocMaxPos = 0;
    let gcMax = 0;
    let mallocMax = 0;
    for (let i = 0; i < numSamples; i++) {
      if (gHistory.gcBytes[i] >= gcMax) {
        gcMax = gHistory.gcBytes[i];
        gcMaxPos = i;
      }
      if (gHistory.mallocBytes[i] >= mallocMax) {
        mallocMax = gHistory.mallocBytes[i];
        mallocMaxPos = i;
      }
    }

    this.range = Math.max(gcMax, mallocMax, gHost.gcAllocTrigger, gHost.mallocTrigger);

    this.drawHBarForBytes(gcMax, "GC max", "#00cf61");
    this.drawHBarForBytes(mallocMax, "Malloc max", "#cc1111");
    this.drawHBarForBytes(gHost.gcAllocTrigger, "GC trigger", "#cc11cc");
    this.drawHBarForBytes(gHost.mallocTrigger, "Malloc trigger", "#cc11cc");

    ctx.fillStyle = "rgb(255,0,0)";

    if (gcMax !== 0) {
      ctx.fillText(
        format_bytes(gcMax),
        this.xpos(gcMaxPos) - 10,
        this.ypos(gcMax) - 14
      );
    }
    if (mallocMax !== 0) {
      ctx.fillText(
        format_bytes(mallocMax),
        this.xpos(mallocMaxPos) - 10,
        this.ypos(mallocMax) - 14
      );
    }

    const where = sampleIndex % numSamples;

    ctx.beginPath();
    ctx.arc(
      this.xpos(where),
      this.ypos(gHistory.gcBytes[where]),
      5,
      0,
      Math.PI * 2,
      true
    );
    ctx.fill();
    ctx.beginPath();
    ctx.arc(
      this.xpos(where),
      this.ypos(gHistory.mallocBytes[where]),
      5,
      0,
      Math.PI * 2,
      true
    );
    ctx.fill();

    ctx.beginPath();
    for (let i = 0; i < numSamples; i++) {
      let x = this.xpos(i);
      let y = this.ypos(gHistory.gcBytes[i]);
      if (i == (sampleIndex + 1) % numSamples) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
      if (i == where) {
        ctx.stroke();
      }
    }
    ctx.stroke();

    ctx.beginPath();
    for (let i = 0; i < numSamples; i++) {
      let x = this.xpos(i);
      let y = this.ypos(gHistory.mallocBytes[i]);
      if (i == (sampleIndex + 1) % numSamples) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
      if (i == where) {
        ctx.stroke();
      }
    }
    ctx.stroke();

    ctx.fillStyle = "rgb(0,0,0)";

    this.drawAxisLabels("Time", "Heap Memory Usage");
  }
};

function onUpdateDisplayChanged() {
  const do_graph = document.getElementById("do-graph");
  if (do_graph.checked) {
    window.requestAnimationFrame(handler);
    gHistory.resume();
  } else {
    gHistory.pause();
  }
  update_load_state_indicator();
}

function onDoLoadChange() {
  const do_load = document.getElementById("do-load");
  gLoadMgr.paused = !do_load.checked;
  console.log(`load paused: ${gLoadMgr.paused}`);
  update_load_state_indicator();
}

var previous = 0;
function handler(timestamp) {
  if (gHistory.is_stopped()) {
    return;
  }

  const completed = gLoadMgr.tick(timestamp);
  if (completed) {
    end_test(timestamp, gLoadMgr.lastActive);
    if (!gLoadMgr.stopped()) {
      start_test();
    }
    update_load_display();
  }

  if (testState == "running") {
    document.getElementById("test-progress").textContent =
      (gLoadMgr.currentLoadRemaining(timestamp) / 1000).toFixed(1) + " sec";
  }

  const delay = gHistory.on_frame(timestamp);

  update_histogram(gHistogram, delay);

  latencyGraph.draw();
  if (memoryGraph) {
    memoryGraph.draw();
  }
  window.requestAnimationFrame(handler);
}

// For interactive debugging.
//
// ['a', 'b', 'b', 'b', 'c', 'c'] => ['a', 'b x 3', 'c x 2']
function summarize(arr) {
  if (!arr.length) {
    return [];
  }

  var result = [];
  var run_start = 0;
  var prev = arr[0];
  for (let i = 1; i <= arr.length; i++) {
    if (i == arr.length || arr[i] != prev) {
      if (i == run_start + 1) {
        result.push(arr[i]);
      } else {
        result.push(prev + " x " + (i - run_start));
      }
      run_start = i;
    }
    if (i != arr.length) {
      prev = arr[i];
    }
  }

  return result;
}

function reset_draw_state() {
  gHistory.reset();
}

function onunload() {
  if (gLoadMgr) {
    gLoadMgr.deactivateLoad();
  }
}

async function onload() {
  // Collect all test loads into the `tests` Map.
  let imports = [];
  foreach_test_file(path => imports.push(import("./" + path)));
  await Promise.all(imports);

  // The order of `tests` is currently based on their asynchronous load
  // order, rather than the listed order. Rearrange by extracting the test
  // names from their filenames, which is kind of gross.
  _tests = tests;
  tests = new Map();
  foreach_test_file(fn => {
    // "benchmarks/foo.js" => "foo"
    const name = fn.split(/\//)[1].split(/\./)[0];
    tests.set(name, _tests.get(name));
  });
  _tests = undefined;

  gLoadMgr = new AllocationLoadManager(tests);

  // Load initial test duration.
  duration_changed();

  // Load initial garbage size.
  garbage_piles_changed();
  garbage_per_frame_changed();

  // Populate the test selection dropdown.
  var select = document.getElementById("test-selection");
  for (var [name, test] of tests) {
    test.name = name;
    var option = document.createElement("option");
    option.id = name;
    option.text = name;
    option.title = test.description;
    select.add(option);
  }

  // Load the initial test.
  gLoadMgr.setActiveLoad(gLoadMgr.getByName("noAllocation"));
  update_load_display();
  document.getElementById("test-selection").value = "noAllocation";

  // Polyfill rAF.
  var requestAnimationFrame =
    window.requestAnimationFrame ||
    window.mozRequestAnimationFrame ||
    window.webkitRequestAnimationFrame ||
    window.msRequestAnimationFrame;
  window.requestAnimationFrame = requestAnimationFrame;

  // Acquire our canvas.
  var canvas = document.getElementById("graph");
  latencyGraph = new LatencyGraph(canvas);

  if (!gHost.features.haveMemorySizes) {
    document.getElementById("memgraph-disabled").style.display = "block";
    document.getElementById("track-sizes-div").style.display = "none";
  }

  trackHeapSizes(document.getElementById("track-sizes").checked);

  update_load_state_indicator();
  gHistory.start();

  // Start drawing.
  reset_draw_state();
  window.requestAnimationFrame(handler);
}

function run_one_test() {
  start_test_cycle([gLoadMgr.activeLoad().name]);
}

function run_all_tests() {
  start_test_cycle([...tests.keys()]);
}

function start_test_cycle(tests_to_run) {
  // Convert from an iterable to an array for pop.
  const duration = gLoadMgr.testDurationMS / 1000;
  const mutators = tests_to_run.map(name => new SingleMutatorSequencer(gLoadMgr.getByName(name), gPerf, duration));
  const sequencer = new ChainSequencer(mutators);
  gLoadMgr.startSequencer(sequencer);
  testState = "running";
  gHistogram.clear();
  reset_draw_state();
}

function update_load_state_indicator() {
  if (
    !gLoadMgr.load_running() ||
    gLoadMgr.activeLoad().name == "noAllocation"
  ) {
    loadState = "(none)";
  } else if (gHistory.is_stopped() || gLoadMgr.paused) {
    loadState = "(inactive)";
  } else {
    loadState = "(active)";
  }
  document.getElementById("load-running").textContent = loadState;
}

function start_test() {
  console.log(`Running test: ${gLoadMgr.activeLoad().name}`);
  document.getElementById("test-selection").value = gLoadMgr.activeLoad().name;
  update_load_state_indicator();
}

function end_test(timestamp, load) {
  document.getElementById("test-progress").textContent = "(not running)";
  report_test_result(load, gHistogram);
  gHistogram.clear();
  console.log(`Ending test ${load.name}`);
  if (gLoadMgr.stopped()) {
    testState = "idle";
  }
  update_load_state_indicator();
  reset_draw_state();
}

function compute_test_spark_histogram(histogram) {
  const percents = compute_spark_histogram_percents(histogram);

  var sparks = "▁▂▃▄▅▆▇█";
  var colors = [
    "#aaaa00",
    "#007700",
    "#dd0000",
    "#ff0000",
    "#ff0000",
    "#ff0000",
    "#ff0000",
    "#ff0000",
  ];
  var line = "";
  for (let i = 0; i < percents.length; ++i) {
    var spark = sparks.charAt(parseInt(percents[i] * sparks.length));
    line += `<span style="color:${colors[i]}">${spark}</span>`;
  }
  return line;
}

function report_test_result(load, histogram) {
  var resultList = document.getElementById("results-display");
  var resultElem = document.createElement("div");
  var score = compute_test_score(histogram);
  var sparks = compute_test_spark_histogram(histogram);
  var params = `(${format_num(load.garbagePerFrame)},${format_num(
    load.garbagePiles
  )})`;
  resultElem.innerHTML = `${score.toFixed(3)} ms/s : ${sparks} : ${
    load.name
  }${params} - ${load.description}`;
  resultList.appendChild(resultElem);
}

function update_load_display() {
  const garbage = gLoadMgr.activeLoad()
    ? gLoadMgr.activeLoad().garbagePerFrame
    : parse_units(gDefaultGarbagePerFrame);
  document.getElementById("garbage-per-frame").value = format_num(garbage);
  const piles = gLoadMgr.activeLoad()
    ? gLoadMgr.activeLoad().garbagePiles
    : parse_units(gDefaultGarbagePiles);
  document.getElementById("garbage-piles").value = format_num(piles);
  update_load_state_indicator();
}

function duration_changed() {
  var durationInput = document.getElementById("test-duration");
  gLoadMgr.testDurationMS = parseInt(durationInput.value) * 1000;
  console.log(
    `Updated test duration to: ${gLoadMgr.testDurationMS / 1000} seconds`
  );
}

function onLoadChange() {
  var select = document.getElementById("test-selection");
  console.log(`Switching to test: ${select.value}`);
  gLoadMgr.setActiveLoad(gLoadMgr.getByName(select.value));
  update_load_display();
  gHistogram.clear();
  reset_draw_state();
}

function garbage_piles_changed() {
  const input = document.getElementById("garbage-piles");
  const value = parse_units(input.value);
  if (isNaN(value)) {
    update_load_display();
    return;
  }

  if (gLoadMgr.load_running()) {
    gLoadMgr.change_garbagePiles(value);
    console.log(
      `Updated garbage-piles to ${gLoadMgr.activeLoad().garbagePiles} items`
    );
  }
  gHistogram.clear();
  reset_draw_state();
}

function garbage_per_frame_changed() {
  const input = document.getElementById("garbage-per-frame");
  var value = parse_units(input.value);
  if (isNaN(value)) {
    update_load_display();
    return;
  }
  if (gLoadMgr.load_running()) {
    gLoadMgr.change_garbagePerFrame(value);
    console.log(
      `Updated garbage-per-frame to ${
        gLoadMgr.activeLoad().garbagePerFrame
      } items`
    );
  }
}

function trackHeapSizes(track) {
  enabled.trackingSizes = track && gHost.features.haveMemorySizes;

  var canvas = document.getElementById("memgraph");

  if (enabled.trackingSizes) {
    canvas.style.display = "block";
    memoryGraph = new MemoryGraph(canvas);
  } else {
    canvas.style.display = "none";
    memoryGraph = null;
  }
}
