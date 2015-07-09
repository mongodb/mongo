# Debugger.Script

A `Debugger.Script` instance refers to a sequence of bytecode in the
debuggee; it is the [`Debugger`][debugger-object] API's presentation of a JSAPI `JSScript`
object. Each of the following is represented by single JSScript object:

* The body of a function—that is, all the code in the function that is not
  contained within some nested function.

* The code passed to a single call to `eval`, excluding the bodies of any
  functions that code defines.

* The contents of a `<script>` element.

* A DOM event handler, whether embedded in HTML or attached to the element
  by other JavaScript code.

* Code appearing in a `javascript:` URL.

The [`Debugger`][debugger-object] interface constructs `Debugger.Script` objects as scripts
of debuggee code are uncovered by the debugger: via the `onNewScript`
handler method; via [`Debugger.Frame`][frame]'s `script` properties; via the
`functionScript` method of [`Debugger.Object`][object] instances; and so on. For a
given [`Debugger`][debugger-object] instance, SpiderMonkey constructs exactly one
`Debugger.Script` instance for each underlying script object; debugger
code can add its own properties to a script object and expect to find
them later, use `==` to decide whether two expressions refer to the same
script, and so on.

(If more than one [`Debugger`][debugger-object] instance is debugging the same code, each
[`Debugger`][debugger-object] gets a separate `Debugger.Script` instance for a given
script. This allows the code using each [`Debugger`][debugger-object] instance to place
whatever properties it likes on its `Debugger.Script` instances, without
worrying about interfering with other debuggers.)

A `Debugger.Script` instance is a strong reference to a JSScript object;
it protects the script it refers to from being garbage collected.

Note that SpiderMonkey may use the same `Debugger.Script` instances for
equivalent functions or evaluated code—that is, scripts representing the
same source code, at the same position in the same source file,
evaluated in the same lexical environment.

## Accessor Properties of the Debugger.Script Prototype Object

A `Debugger.Script` instance inherits the following accessor properties
from its prototype:

`displayName`
:   The script's display name, if it has one. If the script has no display name
    &mdash; for example, if it is a top-level `eval` script &mdash; this is
    `undefined`.

    If the script's function has a given name, its display name is the same as
    its function's given name.

    If the script's function has no name, SpiderMonkey attempts to infer an
    appropriate name for it given its context. For example:

    ```language-js
    function f() {}          // display name: f (the given name)
    var g = function () {};  // display name: g
    o.p = function () {};    // display name: o.p
    var q = {
      r: function () {}      // display name: q.r
    };
    ```

    Note that the display name may not be a proper JavaScript identifier,
    or even a proper expression: we attempt to find helpful names even when
    the function is not immediately assigned as the value of some variable
    or property. Thus, we use <code><i>a</i>/<i>b</i></code> to refer to
    the <i>b</i> defined within <i>a</i>, and <code><i>a</i>&lt;</code> to
    refer to a function that occurs somewhere within an expression that is
    assigned to <i>a</i>. For example:

    ```language-js
    function h() {
      var i = function() {};    // display name: h/i
      f(function () {});        // display name: h/<
    }
    var s = f(function () {});  // display name: s<
    ```

`url`
:   The filename or URL from which this script's code was loaded. If the
    `source` property is non-`null`, then this is equal to `source.url`.

`startLine`
:   The number of the line at which this script's code starts, within the
    file or document named by `url`.

`lineCount`
:   The number of lines this script's code occupies, within the file or
    document named by `url`.

`source`
:   The [`Debugger.Source`][source] instance representing the source code from which
    this script was produced. This is `null` if the source code was not
    retained.

`sourceStart`
:   The character within the [`Debugger.Source`][source] instance given by `source` at
    which this script's code starts; zero-based. If this is a function's
    script, this is the index of the start of the `function` token in the
    source code.

`sourceLength`
:   The length, in characters, of this script's code within the
    [`Debugger.Source`][source] instance given by `source`.

`global`
:   A [`Debugger.Object`][object] instance referring to the global object in whose
    scope this script runs. The result refers to the global directly, not
    via a wrapper or a `WindowProxy` ("outer window", in Firefox).

`staticLevel`
:   The number of function bodies enclosing this script's code.

    Global code is at level zero; bodies of functions defined at the top
    level in global code are at level one; bodies of functions nested within
    those are at level two; and so on.

    A script for code passed to direct `eval` is at a static level one
    greater than that of the script containing the call to `eval`, because
    direct eval code runs within the caller's scope. However, a script for
    code passed to an indirect `eval` call is at static level zero, since it
    is evaluated in the global scope.

    Note that a generator's expressions are considered to be part of the
    body of a synthetic function, produced by the compiler.

    Scripts' static level be useful in deciding where to set breakpoints.
    For example, a breakpoint set on line 3 in this code:

    ```language-js
    function f() {
      x = function g() {  // line 2
                          // line 3; no code here
        ...;
      }
    }
    ```

    should be set in `g`'s script, not in `f`'s, even though neither script
    contains code at that line. In such a case, the most deeply nested
    script—the one with the highest static level—should receive the
    breakpoint.

`strictMode`
:   This is `true` if this script's code is ECMAScript strict mode code, and
    `false` otherwise.

## Function Properties of the Debugger.Script Prototype Object

The functions described below may only be called with a `this` value
referring to a `Debugger.Script` instance; they may not be used as
methods of other kinds of objects.

<code>decompile([<i>pretty</i>])</code>
:   Return a string containing JavaScript source code equivalent to this
    script in its effect and result. If <i>pretty</i> is present and true,
    produce indented code with line breaks.

    (Note that [`Debugger.Object`][object] instances referring to functions also have
    a `decompile` method, whose result includes the function header and
    parameter names, so it is probably better to write
    <code><i>f</i>.decompile()</code> than to write
    <code><i>f</i>.getFunctionScript().decompile()</code>.)

`getAllOffsets()`
:   Return an array <i>L</i> describing the relationship between bytecode
    instruction offsets and source code positions in this script. <i>L</i>
    is sparse, and indexed by source line number. If a source line number
    <i>line</i> has no code, then <i>L</i> has no <i>line</i> property. If
    there is code for <i>line</i>, then <code><i>L</i>[<i>line</i>]</code> is an array
    of offsets of byte code instructions that are entry points to that line.

    For example, suppose we have a script for the following source code:

    ```language-js
    a=[]
    for (i=1; i < 10; i++)
        // It's hip to be square.
        a[i] = i*i;
    ```

    Calling `getAllOffsets()` on that code might yield an array like this:

    ```language-js
    [[0], [5, 20], , [10]]
    ```

    This array indicates that:

    * the first line's code starts at offset 0 in the script;

    * the `for` statement head has two entry points at offsets 5 and 20 (for
      the initialization, which is performed only once, and the loop test,
      which is performed at the start of each iteration);

    * the third line has no code;

    * and the fourth line begins at offset 10.

`getAllColumnOffsets()`:
:   Return an array describing the relationship between bytecode instruction
    offsets and source code positions in this script. Unlike getAllOffsets(),
    which returns all offsets that are entry points for each line,
    getAllColumnOffsets() returns all offsets that are entry points for each
    (line, column) pair.

    The elements of the array are objects, each of which describes a single
    entry point, and contains the following properties:

    * lineNumber: the line number for which offset is an entry point

    * columnNumber: the column number for which offset is an entry point

    * offset: the bytecode instruction offset of the entry point

    For example, suppose we have a script for the following source code:

    ```language-js
    a=[]
    for (i=1; i < 10; i++)
        // It's hip to be square.
        a[i] = i*i;
    ```

    Calling `getAllColumnOffsets()` on that code might yield an array like this:

    ```language-js
    [{ lineNumber: 0, columnNumber: 0, offset: 0 },
     { lineNumber: 1, columnNumber: 5, offset: 5 },
     { lineNumber: 1, columnNumber: 10, offset: 20 },
     { lineNumber: 3, columnNumber: 4, offset: 10 }]

<code>getLineOffsets(<i>line</i>)</code>
:   Return an array of bytecode instruction offsets representing the entry
    points to source line <i>line</i>. If the script contains no executable
    code at that line, the array returned is empty.

<code>getOffsetLine(<i>offset</i>)</code>
:   Return the source code line responsible for the bytecode at
    <i>offset</i> in this script.

`getChildScripts()`
:   Return a new array whose elements are Debugger.Script objects for each
    function and each generator expression in this script. Only direct
    children are included; nested children can be reached by walking the
    tree.

<code>setBreakpoint(<i>offset</i>, <i>handler</i>)</code>
:   Set a breakpoint at the bytecode instruction at <i>offset</i> in this
    script, reporting hits to the `hit` method of <i>handler</i>. If
    <i>offset</i> is not a valid offset in this script, throw an error.

    When execution reaches the given instruction, SpiderMonkey calls the
    `hit` method of <i>handler</i>, passing a [`Debugger.Frame`][frame]
    instance representing the currently executing stack frame. The `hit`
    method's return value should be a [resumption value][rv], determining
    how execution should continue.

    Any number of breakpoints may be set at a single location; when control
    reaches that point, SpiderMonkey calls their handlers in an unspecified
    order.

    Any number of breakpoints may use the same <i>handler</i> object.

    Breakpoint handler method calls are cross-compartment, intra-thread
    calls: the call takes place in the same thread that hit the breakpoint,
    and in the compartment containing the handler function (typically the
    debugger's compartment).

    The new breakpoint belongs to the [`Debugger`][debugger-object] instance to
    which this script belongs. Disabling the [`Debugger`][debugger-object]
    instance disables this breakpoint; and removing a global from the
    [`Debugger`][debugger-object] instance's set of debuggees clears all the
    breakpoints belonging to that [`Debugger`][debugger-object] instance in that
    global's scripts.

<code>getBreakpoints([<i>offset</i>])</code>
:   Return an array containing the handler objects for all the breakpoints
    set at <i>offset</i> in this script. If <i>offset</i> is omitted, return
    the handlers of all breakpoints set anywhere in this script. If
    <i>offset</i> is present, but not a valid offset in this script, throw
    an error.

<code>clearBreakpoints(handler, [<i>offset</i>])</code>
:   Remove all breakpoints set in this [`Debugger`][debugger-object] instance that use
    <i>handler</i> as their handler. If <i>offset</i> is given, remove only
    those breakpoints set at <i>offset</i> that use <i>handler</i>; if
    <i>offset</i> is not a valid offset in this script, throw an error.

    Note that, if breakpoints using other handler objects are set at the
    same location(s) as <i>handler</i>, they remain in place.

<code>clearAllBreakpoints([<i>offset</i>])</code>
:   Remove all breakpoints set in this script. If <i>offset</i> is present,
    remove all breakpoints set at that offset in this script; if
    <i>offset</i> is not a valid bytecode offset in this script, throw an
    error.

<code>isInCatchScope([<i>offset</i>])</code>
:   This is `true` if this offset falls within the scope of a try block, and
    `false` otherwise.
