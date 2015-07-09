# The `Debugger` Interface

Mozilla's JavaScript engine, SpiderMonkey, provides a debugging interface
named `Debugger` which lets JavaScript code observe and manipulate the
execution of other JavaScript code. Both Firefox's built-in developer tools
and the Firebug add-on use `Debugger` to implement their JavaScript
debuggers. However, `Debugger` is quite general, and can be used to
implement other kinds of tools like tracers, coverage analysis,
patch-and-continue, and so on.

`Debugger` has three essential qualities:

- It is a *source level* interface: it operates in terms of the JavaScript
  language, not machine language. It operates on JavaScript objects, stack
  frames, environments, and code, and presents a consistent interface
  regardless of whether the debuggee is interpreted, compiled, or
  optimized. If you have a strong command of the JavaScript language, you
  should have all the background you need to use `Debugger` successfully,
  even if you have never looked into the language's implementation.

- It is for use *by JavaScript code*. JavaScript is both the debuggee
  language and the tool implementation language, so the qualities that make
  JavaScript effective on the web can be brought to bear in crafting tools
  for developers. As is expected of JavaScript APIs, `Debugger` is a
  *sound* interface: using (or even misusing) `Debugger` should never cause
  Gecko to crash. Errors throw proper JavaScript exceptions.

- It is an *intra-thread* debugging API. Both the debuggee and the code
  using `Debugger` to observe it must run in the same thread. Cross-thread,
  cross-process, and cross-device tools must use `Debugger` to observe the
  debuggee from within the same thread, and then handle any needed
  communication themselves. (Firefox's builtin tools have a
  [protocol][protocol] defined for this purpose.)

In Gecko, the `Debugger` API is available to chrome code only. By design,
it ought not to introduce security holes, so in principle it could be made
available to content as well; but it is hard to justify the security risks
of the additional attack surface.


## Debugger Instances and Shadow Objects

`Debugger` reflects every aspect of the debuggee's state as a JavaScript
value---not just actual JavaScript values like objects and primitives,
but also stack frames, environments, scripts, and compilation units, which
are not normally accessible as objects in their own right.

Here is a JavaScript program in the process of running a timer callback function:

![A running JavaScript program and its Debugger shadows][img-shadows]

This diagram shows the various types of shadow objects that make up the
Debugger API (which all follow some [general conventions][conventions]):

- A [`Debugger.Object`][object] represents a debuggee object, offering a
  reflection-oriented API that protects the debugger from accidentally
  invoking getters, setters, proxy traps, and so on.

- A [`Debugger.Script`][script] represents a block of JavaScript
  code---either a function body or a top-level script. Given a
  `Debugger.Script`, one can set breakpoints, translate between source
  positions and bytecode offsets (a deviation from the "source level"
  design principle), and find other static characteristics of the code.

- A [`Debugger.Frame`][frame] represents a running stack frame. You can use
  these to walk the stack and find each frame's script and environment. You
  can also set `onStep` and `onPop` handlers on frames.

- A [`Debugger.Environment`][environment] represents an environment,
  associating variable names with storage locations. Environments may
  belong to a running stack frame, captured by a function closure, or
  reflect some global object's properties as variables.

The [`Debugger`][debugger-object] instance itself is not really a shadow of
anything in the debuggee; rather, it maintains the set of global objects
which are to be considered debuggees. A `Debugger` observes only execution
taking place in the scope of these global objects. You can set functions to
be called when new stack frames are pushed; when new code is loaded; and so
on.

Omitted from this picture are [`Debugger.Source`][source] instances, which
represent JavaScript compilation units. A `Debugger.Source` can furnish a
full copy of its source code, and explain how the code entered the system,
whether via a call to `eval`, a `<script>` element, or otherwise. A
`Debugger.Script` points to the `Debugger.Source` from which it is derived.

Also omitted is the `Debugger`'s [`Debugger.Memory`][memory] instance, which
holds methods and accessors for observing the debuggee's memory use.

All these types follow some [general conventions][conventions], which you
should look through before drilling down into any particular type's
specification.

All shadow objects are unique per `Debugger` and per referent. For a given
`Debugger`, there is exactly one `Debugger.Object` that refers to a
particular debuggee object; exactly one `Debugger.Frame` for a particular
stack frame; and so on. Thus, a tool can store metadata about a shadow's
referent as a property on the shadow itself, and count on finding that
metadata again if it comes across the same referent. And since shadows are
per-`Debugger`, tools can do so without worrying about interfering with
other tools that use their own `Debugger` instances.


## Examples

Here are some things you can try out yourself that show off some of `Debugger`'s
features:

- [Evaluating an expression in a web page's stack frame when it executes a `debugger;` statement.][tut debugger]

- [Showing how many objects different call paths allocate.][tut alloc log]


## Gecko-specific features

While the `Debugger` core API deals only with concepts common to any
JavaScript implementation, it also includes some Gecko-specific features:

- [Global tracking][global] supports debugging all the code running in a
  Gecko instance at once---the 'chrome debugging' model.
- [Object wrapper][wrapper] functions help manipulate object references
  that cross privilege boundaries.
