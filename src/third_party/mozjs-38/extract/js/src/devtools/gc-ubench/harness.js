// Per-frame time sampling infra. Also GC'd: hopefully will not perturb things too badly.
var numSamples = 500;
var delays = new Array(numSamples);
var sampleIndex = 0;
var sampleTime = 16; // ms
var gHistogram = new Map(); // {ms: count}

// Draw state.
var stopped = 0;
var start;
var prev;
var ctx;

// Current test state.
var activeTest = undefined;
var testDuration = undefined; // ms
var testState = 'idle';  // One of 'idle' or 'running'.
var testStart = undefined; // ms
var testQueue = [];

// Global defaults
var globalDefaultGarbageTotal = "8M";
var globalDefaultGarbagePerFrame = "8K";

function xpos(index)
{
    return index * 2;
}

function ypos(delay)
{
    var r = 525 - Math.log(delay) * 64;
    if (r < 5) return 5;
    return r;
}

function drawHBar(delay, color, label)
{
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    ctx.fillText(label, xpos(numSamples) + 4, ypos(delay) + 3);

    ctx.beginPath();
    ctx.moveTo(xpos(0), ypos(delay));
    ctx.lineTo(xpos(numSamples), ypos(delay));
    ctx.stroke();
    ctx.strokeStyle = 'rgb(0,0,0)';
    ctx.fillStyle = 'rgb(0,0,0)';
}

function drawScale(delay)
{
    drawHBar(delay, 'rgb(150,150,150)', `${delay}ms`);
}

function draw60fps() {
    drawHBar(1000/60, '#00cf61', '60fps');
}

function draw30fps() {
    drawHBar(1000/30, '#cf0061', '30fps');
}

function drawGraph()
{
    ctx.clearRect(0, 0, 1100, 550);

    drawScale(10);
    draw60fps();
    drawScale(20);
    drawScale(30);
    draw30fps();
    drawScale(50);
    drawScale(100);
    drawScale(200);
    drawScale(400);
    drawScale(800);

    var worst = 0, worstpos = 0;
    ctx.beginPath();
    for (var i = 0; i < numSamples; i++) {
        ctx.lineTo(xpos(i), ypos(delays[i]));
        if (delays[i] >= worst) {
            worst = delays[i];
            worstpos = i;
        }
    }
    ctx.stroke();

    ctx.fillStyle = 'rgb(255,0,0)';
    if (worst)
        ctx.fillText(''+worst.toFixed(2)+'ms', xpos(worstpos) - 10, ypos(worst) - 14);

    ctx.beginPath();
    var where = sampleIndex % numSamples;
    ctx.arc(xpos(where), ypos(delays[where]), 5, 0, Math.PI*2, true);
    ctx.fill();
    ctx.fillStyle = 'rgb(0,0,0)';

    ctx.fillText('Time', 550, 420);
    ctx.save();
    ctx.rotate(Math.PI/2);
    ctx.fillText('Pause between frames (log scale)', 150, -1060);
    ctx.restore();
}

function stopstart()
{
    if (stopped) {
        window.requestAnimationFrame(handler);
        prev = performance.now();
        start += prev - stopped;
        document.getElementById('stop').value = 'Pause';
        stopped = 0;
    } else {
        document.getElementById('stop').value = 'Resume';
        stopped = performance.now();
    }
}

function handler(timestamp)
{
    if (stopped)
        return;

    if (testState === 'running' && (timestamp - testStart) > testDuration)
        end_test(timestamp);

    if (testState == 'running')
        document.getElementById("test-progress").textContent = ((testDuration - (timestamp - testStart))/1000).toFixed(1) + " sec";

    activeTest.makeGarbage(activeTest.garbagePerFrame);

    var elt = document.getElementById('data');
    var delay = timestamp - prev;
    prev = timestamp;

    // Take the histogram at 10us intervals so that we have enough resolution to capture.
    // a 16.66[666] target with adequate accuracy.
    update_histogram(gHistogram, Math.round(delay * 100));

    var t = timestamp - start;
    var newIndex = Math.round(t / sampleTime);
    while (sampleIndex < newIndex) {
        sampleIndex++;
        delays[sampleIndex % numSamples] = delay;
    }

    drawGraph();
    window.requestAnimationFrame(handler);
}

function update_histogram(histogram, delay)
{
    var current = histogram.has(delay) ? histogram.get(delay) : 0;
    histogram.set(delay, ++current);
}

function reset_draw_state()
{
    for (var i = 0; i < numSamples; i++)
        delays[i] = 0;
    start = prev = performance.now();
    sampleIndex = 0;
}

function onunload()
{
    if (activeTest)
        activeTest.unload();
    activeTest = undefined;
}

function onload()
{
    // Load initial test duration.
    duration_changed();

    // Load initial garbage size.
    garbage_total_changed();
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
    change_active_test('noAllocation');

    // Polyfill rAF.
    var requestAnimationFrame =
        window.requestAnimationFrame || window.mozRequestAnimationFrame ||
        window.webkitRequestAnimationFrame || window.msRequestAnimationFrame;
    window.requestAnimationFrame = requestAnimationFrame;

    // Acquire our canvas.
    var canvas = document.getElementById('graph');
    ctx = canvas.getContext('2d');

    // Start drawing.
    reset_draw_state();
    window.requestAnimationFrame(handler);
}

function run_one_test()
{
    start_test_cycle([activeTest.name]);
}

function run_all_tests()
{
    start_test_cycle(tests.keys());
}

function start_test_cycle(tests_to_run)
{
    // Convert from an iterable to an array for pop.
    testQueue = [];
    for (var key of tests_to_run)
        testQueue.push(key);
    testState = 'running';
    testStart = performance.now();
    gHistogram.clear();

    start_test(testQueue.shift());
    reset_draw_state();
}

function start_test(testName)
{
    change_active_test(testName);
    console.log(`Running test: ${testName}`);
    document.getElementById("test-selection").value = testName;
}

function end_test(timestamp)
{
    document.getElementById("test-progress").textContent = "(not running)";
    report_test_result(activeTest, gHistogram);
    gHistogram.clear();
    console.log(`Ending test ${activeTest.name}`);
    if (testQueue.length) {
        start_test(testQueue.shift());
        testStart = timestamp;
    } else {
        testState = 'idle';
        testStart = 0;
    }
    reset_draw_state();
}

function report_test_result(test, histogram)
{
    var resultList = document.getElementById('results-display');
    var resultElem = document.createElement("div");
    var score = compute_test_score(histogram);
    var sparks = compute_test_spark_histogram(histogram);
    var params = `(${format_units(test.garbagePerFrame)},${format_units(test.garbageTotal)})`;
    resultElem.innerHTML = `${score.toFixed(3)} ms/s : ${sparks} : ${test.name}${params} - ${test.description}`;
    resultList.appendChild(resultElem);
}

// Compute a score based on the total ms we missed frames by per second.
function compute_test_score(histogram)
{
    var score = 0;
    for (var [delay, count] of histogram) {
        delay = delay / 100;
        score += Math.abs((delay - 16.66) * count);
    }
    score = score / (testDuration / 1000);
    return Math.round(score * 1000) / 1000;
}

// Build a spark-lines histogram for the test results to show with the aggregate score.
function compute_test_spark_histogram(histogram)
{
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
    for (var [delay, count] of histogram) {
        delay = delay / 100;
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
    for (var [i, count] of rescaled)
        total += count;
    var sparks = "▁▂▃▄▅▆▇█";
    var colors = ['#aaaa00', '#007700', '#dd0000', '#ff0000',
                  '#ff0000', '#ff0000', '#ff0000', '#ff0000'];
    var line = "";
    for (var i = 0; i < ranges.length; ++i) {
        var amt = rescaled.has(i) ? rescaled.get(i) : 0;
        var spark = sparks.charAt(parseInt(amt/total*8));
        line += `<span style="color:${colors[i]}">${spark}</span>`;
    }
    return line;
}

function reload_active_test()
{
    activeTest.unload();
    activeTest.load(activeTest.garbageTotal);
}

function change_active_test(new_test_name)
{
    if (activeTest)
        activeTest.unload();
    activeTest = tests.get(new_test_name);

    if (!activeTest.garbagePerFrame)
        activeTest.garbagePerFrame = activeTest.defaultGarbagePerFrame || globalDefaultGarbagePerFrame;
    if (!activeTest.garbageTotal)
        activeTest.garbageTotal = activeTest.defaultGarbageTotal || globalDefaultGarbageTotal;

    document.getElementById("garbage-per-frame").value = format_units(activeTest.garbagePerFrame);
    document.getElementById("garbage-total").value = format_units(activeTest.garbageTotal);

    activeTest.load(activeTest.garbageTotal);
}

function duration_changed()
{
    var durationInput = document.getElementById('test-duration');
    testDuration = parseInt(durationInput.value) * 1000;
    console.log(`Updated test duration to: ${testDuration / 1000} seconds`);
}

function test_changed()
{
    var select = document.getElementById("test-selection");
    console.log(`Switching to test: ${select.value}`);
    change_active_test(select.value);
    gHistogram.clear();
    reset_draw_state();
}

function parse_units(v)
{
    if (v.length == 0)
        return NaN;
    var lastChar = v[v.length - 1].toLowerCase();
    if (!isNaN(parseFloat(lastChar)))
        return parseFloat(v);
    var units = parseFloat(v.substr(0, v.length - 1));
    if (lastChar == "k")
        return units * 1e3;
    if (lastChar == "m")
        return units * 1e6;
    if (lastChar == "g")
        return units * 1e9;
    return NaN;
}

function format_units(n)
{
    n = String(n);
    if (n.length > 9 && n.substr(-9) == "000000000")
        return n.substr(0, n.length - 9) + "G";
    else if (n.length > 9 && n.substr(-6) == "000000")
        return n.substr(0, n.length - 6) + "M";
    else if (n.length > 3 && n.substr(-3) == "000")
        return n.substr(0, n.length - 3) + "K";
    else
        return String(n);
}

function garbage_total_changed()
{
    var value = parse_units(document.getElementById('garbage-total').value);
    if (isNaN(value))
        return;
    if (activeTest) {
        activeTest.garbageTotal = value;
        console.log(`Updated garbage-total to ${activeTest.garbageTotal} items`);
        reload_active_test();
    }
    gHistogram.clear();
    reset_draw_state();
}

function garbage_per_frame_changed()
{
    var value = parse_units(document.getElementById('garbage-per-frame').value);
    if (isNaN(value))
        return;
    if (activeTest) {
        activeTest.garbagePerFrame = value;
        console.log(`Updated garbage-per-frame to ${activeTest.garbagePerFrame} items`);
    }
}
