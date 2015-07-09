# Debugger.Frame

A `Debugger.Frame` instance represents a [visible stack frame][vf]. Given a
`Debugger.Frame` instance, you can find the script the frame is executing,
walk the stack to older frames, find the lexical environment in which the
execution is taking place, and so on.

For a given [`Debugger`][debugger-object] instance, SpiderMonkey creates
only one `Debugger.Frame` instance for a given visible frame. Every handler
method called while the debuggee is running in a given frame is given the
same frame object. Similarly, walking the stack back to a previously
accessed frame yields the same frame object as before. Debugger code can
add its own properties to a frame object and expect to find them later, use
`==` to decide whether two expressions refer to the same frame, and so on.

(If more than one [`Debugger`][debugger-object] instance is debugging the
same code, each [`Debugger`][debugger-object] gets a separate
`Debugger.Frame` instance for a given frame. This allows the code using
each [`Debugger`][debugger-object] instance to place whatever properties it
likes on its `Debugger.Frame` instances, without worrying about interfering
with other debuggers.)

When the debuggee pops a stack frame (say, because a function call has
returned or an exception has been thrown from it), the `Debugger.Frame`
instance referring to that frame becomes inactive: its `live` property
becomes `false`, and accessing its other properties or calling its methods
throws an exception. Note that frames only become inactive at times that
are predictable for the debugger: when the debuggee runs, or when the
debugger removes frames from the stack itself.

Stack frames that represent the control state of generator-iterator objects
behave in a special way, described in [Generator Frames][generator] below.


## Visible Frames

When inspecting the call stack, [`Debugger`][debugger-object] does not
reveal all the frames that are actually present on the stack: while it does
reveal all frames running debuggee code, it omits frames running the
debugger's own code, and omits most frames running non-debuggee code. We
call those stack frames a [`Debugger`][debugger-object] does reveal
<i>visible frames</i>.

A frame is a visible frame if any of the following are true:

* it is running [debuggee code][dbg code];

* its immediate caller is a frame running debuggee code; or

* it is a [`"debugger"` frame][inv fr],
  representing the continuation of debuggee code invoked by the debugger.

The "immediate caller" rule means that, when debuggee code calls a
non-debuggee function, it looks like a call to a primitive: you see a frame
for the non-debuggee function that was accessible to the debuggee, but any
further calls that function makes are treated as internal details, and
omitted from the stack trace. If the non-debuggee function eventually calls
back into debuggee code, then those frames are visible.

(Note that the debuggee is not considered an "immediate caller" of handler
methods it triggers. Even though the debuggee and debugger share the same
JavaScript stack, frames pushed for SpiderMonkey's calls to handler methods
to report events in the debuggee are never considered visible frames.)


## <span id='invf'>Invocation</span> Functions and "debugger" Frames

An <i>invocation function</i> is any function in this interface that allows
the debugger to invoke code in the debuggee:
`Debugger.Object.prototype.call`, `Debugger.Frame.prototype.eval`, and so
on.

While invocation functions differ in the code to be run and how to pass
values to it, they all follow this general procedure:

1. Let <i>older</i> be the youngest visible frame on the stack, or `null`
   if there is no such frame. (This is never one of the the debugger's own
   frames; those never appear as `Debugger.Frame` instances.)

2. Push a `"debugger"` frame on the stack, with <i>older</i> as its
   `older` property.

3. Invoke the debuggee code as appropriate for the given invocation
   function, with the `"debugger"` frame as its continuation. For example,
   `Debugger.Frame.prototype.eval` pushes an `"eval"` frame for code it
   runs, whereas `Debugger.Object.prototype.call` pushes a `"call"` frame.

4. When the debuggee code completes, whether by returning, throwing an
   exception or being terminated, pop the `"debugger"` frame, and return an
   appropriate [completion value][cv] from the invocation function to the
   debugger.

When a debugger calls an invocation function to run debuggee code, that
code's continuation is the debugger, not the next debuggee code frame.
Pushing a `"debugger"` frame makes this continuation explicit, and makes it
easier to find the extent of the stack created for the invocation.


## Accessor Properties of the Debugger.Frame Prototype Object

A `Debugger.Frame` instance inherits the following accessor properties from
its prototype:

`type`
:   A string describing what sort of frame this is:

    * `"call"`: a frame running a function call. (We may not be able to obtain
      frames for calls to host functions.)

    * `"eval"`: a frame running code passed to `eval`.

    * `"global"`: a frame running global code (JavaScript that is neither of
      the above).

    * `"debugger"`: a frame for a call to user code invoked by the debugger
      (see the `eval` method below).

`implementation`
:   A string describing which tier of the JavaScript engine this frame is
    executing in:

    * `"interpreter"`: a frame running in the interpreter.

    * `"baseline"`: a frame running in the unoptimizing, baseline JIT.

    * `"ion"`: a frame running in the optimizing JIT.

`this`
:   The value of `this` for this frame (a debuggee value).

`older`
:   The next-older visible frame, in which control will resume when this
    frame completes. If there is no older frame, this is `null`. (On a
    suspended generator frame, the value of this property is `null`; see
    [Generator Frames][generator].)

`depth`
:   The depth of this frame, counting from oldest to youngest; the oldest
    frame has a depth of zero.

`live`
:   True if the frame this `Debugger.Frame` instance refers to is still on
    the stack (or, in the case of generator-iterator objects, has not yet
    finished its iteration); false if it has completed execution or been
    popped in some other way.

`script`
:   The script being executed in this frame (a [`Debugger.Script`][script]
    instance), or `null` on frames that do not represent calls to debuggee
    code. On frames whose `callee` property is not null, this is equal to
    `callee.script`.

`offset`
:   The offset of the bytecode instruction currently being executed in
    `script`, or `undefined` if the frame's `script` property is `null`.

`environment`
:   The lexical environment within which evaluation is taking place (a
    [`Debugger.Environment`][environment] instance), or `null` on frames
    that do not represent the evaluation of debuggee code, like calls
    non-debuggee functions, host functions or `"debugger"` frames.

`callee`
:   The function whose application created this frame, as a debuggee value,
    or `null` if this is not a `"call"` frame.

`generator`
:   True if this frame is a generator frame, false otherwise.

`constructing`
:   True if this frame is for a function called as a constructor, false
    otherwise.

`arguments`
:   The arguments passed to the current frame, or `null` if this is not a
    `"call"` frame. When non-`null`, this is an object, allocated in the
    same global as the debugger, with `Array.prototype` on its prototype
    chain, a non-writable `length` property, and properties whose names are
    array indices. Each property is a read-only accessor property whose
    getter returns the current value of the corresponding parameter. When
    the referent frame is popped, the argument value's properties' getters
    throw an error.


## Handler Methods of Debugger.Frame Instances

Each `Debugger.Frame` instance inherits accessor properties holding handler
functions for SpiderMonkey to call when given events occur in the frame.

Calls to frames' handler methods are cross-compartment, intra-thread calls:
the call takes place in the thread to which the frame belongs, and runs in
the compartment to which the handler method belongs.

`Debugger.Frame` instances inherit the following handler method properties:

`onStep`
:   This property must be either `undefined` or a function. If it is a
    function, SpiderMonkey calls it when execution in this frame makes a
    small amount of progress, passing no arguments and providing this
    `Debugger.Frame` instance as the `this`value. The function should
    return a [resumption value][rv] specifying how the debuggee's execution
    should proceed.

    What constitutes "a small amount of progress" varies depending on the
    implementation, but it is fine-grained enough to implement useful
    "step" and "next" behavior.

    If multiple [`Debugger`][debugger-object] instances each have
    `Debugger.Frame` instances for a given stack frame with `onStep`
    handlers set, their handlers are run in an unspecified order. If any
    `onStep` handler forces the frame to return early (by returning a
    resumption value other than `undefined`), any remaining debuggers'
    `onStep` handlers do not run.

    This property is ignored on frames that are not executing debuggee
    code, like `"call"` frames for calls to host functions and `"debugger"`
    frames.

`onPop`
:   This property must be either `undefined` or a function. If it is a
    function, SpiderMonkey calls it just before this frame is popped,
    passing a [completion value][cv] indicating how this frame's execution
    completed, and providing this `Debugger.Frame` instance as the `this`
    value. The function should return a [resumption value][rv] indicating
    how execution should proceed. On newly created frames, this property's
    value is `undefined`.

    When an `onPop` call reports the completion of a construction call
    (that is, a function called via the `new` operator), the completion
    value passed to the handler describes the value returned by the
    function body. If this value is not an object, it may be different from
    the value produced by the `new` expression, which will be the value of
    the frame's `this` property. (In ECMAScript terms, the `onPop` handler
    receives the value returned by the `[[Call]]` method, not the value
    returned by the `[[Construct]]` method.)

    When a debugger handler function forces a frame to complete early, by
    returning a `{ return:... }`, `{ throw:... }`, or `null` resumption
    value, SpiderMonkey calls the frame's `onPop` handler, if any. The
    completion value passed in this case reflects the resumption value that
    caused the frame to complete.

    When SpiderMonkey calls an `onPop` handler for a frame that is throwing
    an exception or being terminated, and the handler returns `undefined`,
    then SpiderMonkey proceeds with the exception or termination. That is,
    an `undefined` resumption value leaves the frame's throwing and
    termination process undisturbed.

    <i>(Not yet implemented.)</i> When a generator frame yields a value,
    SpiderMonkey calls its `Debugger.Frame` instance's `onPop` handler
    method, if present, passing a `yield` resumption value; however, the
    `Debugger.Frame` instance remains live.

    If multiple [`Debugger`][debugger-object] instances each have
    `Debugger.Frame` instances for a given stack frame with `onPop`
    handlers set, their handlers are run in an unspecified order. The
    resumption value each handler returns establishes the completion value
    reported to the next handler.

    This handler is not called on `"debugger"` frames. It is also not called
    when unwinding a frame due to an over-recursion or out-of-memory
    exception.

`onResume`
:   This property must be either `undefined` or a function. If it is a
    function, SpiderMonkey calls it if the current frame is a generator
    frame whose execution has just been resumed. The function should return
    a [resumption value][rv] indicating how execution should proceed. On
    newly created frames, this property's value is `undefined`.

    If the program resumed the generator by calling its `send` method and
    passing a value, then <i>value</i> is that value. Otherwise,
    <i>value</i> is `undefined`.


## Function Properties of the Debugger.Frame Prototype Object

The functions described below may only be called with a `this` value
referring to a `Debugger.Frame` instance; they may not be used as
methods of other kinds of objects.

<code id="eval">eval(<i>code</i>, [<i>options</i>])</code>
:   Evaluate <i>code</i> in the execution context of this frame, and return
    a [completion value][cv] describing how it completed. <i>Code</i> is a
    string. If this frame's `environment` property is `null`, throw a
    `TypeError`. All extant handler methods, breakpoints, watchpoints, and
    so on remain active during the call. This function follows the
    [invocation function conventions][inv fr].

    <i>Code</i> is interpreted as strict mode code when it contains a Use
    Strict Directive, or the code executing in this frame is strict mode
    code.

    If <i>code</i> is not strict mode code, then variable declarations in
    <i>code</i> affect the environment of this frame. (In the terms used by
    the ECMAScript specification, the `VariableEnvironment` of the
    execution context for the eval code is the `VariableEnvironment` of the
    execution context that this frame represents.) If implementation
    restrictions prevent SpiderMonkey from extending this frame's
    environment as requested, this call throws an Error exception.

    If given, <i>options</i> should be an object whose properties specify
    details of how the evaluation should occur. The `eval` method
    recognizes the following properties:

    <code>url</code>
    :   The filename or URL to which we should attribute <i>code</i>. If this
        property is omitted, the URL defaults to `"debugger eval code"`.

    <code>lineNumber</code>
    :   The line number at which the evaluated code should be claimed to begin
        within <i>url</i>.

<code>evalWithBindings(<i>code</i>, <i>bindings</i>, [<i>options</i>])</code>
:   Like `eval`, but evaluate <i>code</i> in the environment of this frame,
    extended with bindings from the object <i>bindings</i>. For each own
    enumerable property of <i>bindings</i> named <i>name</i> whose value is
    <i>value</i>, include a variable in the environment in which
    <i>code</i> is evaluated named <i>name</i>, whose value is
    <i>value</i>. Each <i>value</i> must be a debuggee value. (This is not
    like a `with` statement: <i>code</i> may access, assign to, and delete
    the introduced bindings without having any effect on the
    <i>bindings</i> object.)

    This method allows debugger code to introduce temporary bindings that
    are visible to the given debuggee code and which refer to debugger-held
    debuggee values, and do so without mutating any existing debuggee
    environment.

    Note that, like `eval`, declarations in the <i>code</i> passed to
    `evalWithBindings` affect the environment of this frame, even as that
    environment is extended by bindings visible within <i>code</i>. (In the
    terms used by the ECMAScript specification, the `VariableEnvironment`
    of the execution context for the eval code is the `VariableEnvironment`
    of the execution context that this frame represents, and the
    <i>bindings</i> appear in a new declarative environment, which is the
    eval code's `LexicalEnvironment`.) If implementation restrictions
    prevent SpiderMonkey from extending this frame's environment as
    requested, this call throws an `Error` exception.

    The <i>options</i> argument is as for
    [`Debugger.Frame.prototype.eval`][fr eval], described above.


## Generator Frames

<i>Not all behavior described in this section has been implemented
yet.</i>

SpiderMonkey supports generator-iterator objects, which produce a series of
values by repeatedly suspending the execution of a function or expression.
For example, calling a function that uses `yield` produces a
generator-iterator object, as does evaluating a generator expression like
`(i*i for each (i in [1,2,3]))`.

A generator-iterator object refers to a stack frame with no fixed
continuation frame. While the generator's code is running, its continuation
is whatever frame called its `next` method; while the generator is
suspended, it has no particular continuation frame; and when it resumes
again, the continuation frame for that resumption could be different from
that of the previous resumption.

A `Debugger.Frame` instance representing a generator frame differs from an
ordinary stack frame as follows:

* A generator frame's `generator` property is true.

* A generator frame disappears from the stack each time the generator
  yields a value and is suspended, and reappears atop the stack when it is
  resumed to produce the generator's next value. The same `Debugger.Frame`
  instance refers to the generator frame until it returns, throws an
  exception, or is terminated.

* A generator frame's `older` property refers to the frame's continuation
  frame while the generator is running, and is `null` while the generator
  is suspended.

* A generator frame's `depth` property reflects the frame's position on
  the stack when the generator is resumed, and is `null` while the
  generator is suspended.

* A generator frame's `live` property remains true until the frame
  returns, throws an exception, or is terminated. Thus, generator frames
  can be live while not present on the stack.

The other `Debugger.Frame` methods and accessor properties work as
described on generator frames, even when the generator frame is suspended.
You may examine a suspended generator frame's variables, and use its
`script` and `offset` members to see which `yield` it is suspended at.

A `Debugger.Frame` instance referring to a generator-iterator frame has a
strong reference to the generator-iterator object; the frame (and its
object) will live as long as the `Debugger.Frame` instance does. However,
when the generator function returns, throws an exception, or is terminated,
thus ending the iteration, the `Debugger.Frame` instance becomes inactive
and its `live` property becomes `false`, just as would occur for any other
sort of frame that is popped. A non-live `Debugger.Frame` instance no
longer holds a strong reference to the generator-iterator object.
