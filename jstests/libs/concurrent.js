/* NOTE: Requires mongo shell to be built with V8 javascript engine,
which implements concurrent threads via fork() */

// Fork and start
function fork_(thunk) {
    thread = fork(thunk)
    thread.start()
    return thread
}

// In functional form, useful for high-order functions like map in fun.js
function join_(thread) {thread.join()}

// Fork a loop on each one-arg block and wait for all of them to terminate. Foreground blocks are executed n times, background blocks are executed repeatedly until all forground loops finish. If any fail, stop all loops and reraise exception in main thread
function parallel(n, foregroundBlock1s, backgroundBlock1s) {
    var err = null
    var stop = false
    function loop(m) {return function(block1) {return function() {
        for (var i = 0; i < m; i++) {if (stop) break; block1(i)} }}}
    function watch(block) {return function() {
        try {block()} catch(e) {err = e; stop = true}}}
    foreThunks = map(watch, map(loop(n), foregroundBlock1s))
    backThunks = map(watch, map(loop(Infinity), backgroundBlock1s))
    foreThreads = map(fork_, foreThunks)
    backThreads = map(fork_, backThunks)
    map(join_, foreThreads)
    stop = true
    map(join_, backThreads)
    if (err != null) throw err
}
