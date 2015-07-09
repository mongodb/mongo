# General Conventions

This page describes general conventions used in the [`Debugger`][debugger] API,
and defines some terminology used throughout the specification.


## Properties

Properties of objects that comprise the `Debugger` interface, and those
that the interface creates, follow some general conventions:

- Instances and prototypes are extensible; you can add your own properties
  and methods to them.

- Properties are configurable. This applies to both "own" and prototype
  properties, and to both methods and data properties. (Leaving these
  properties open to redefinition will hopefully make it easier for
  JavaScript debugger code to cope with bugs, bug fixes, and changes in the
  interface over time.)

- Method properties are writable.

- We prefer inherited accessor properties to own data properties. Both are
  read using the same syntax, but inherited accessors seem like a more
  accurate reflection of what's going on. Unless otherwise noted, these
  properties have getters but no setters, as they cannot meaningfully be
  assigned to.


## Debuggee Values

The `Debugger` interface follows some conventions to help debuggers safely
inspect and modify the debuggee's objects and values. Primitive values are
passed freely between debugger and debuggee; copying or wrapping is handled
transparently. Objects received from the debuggee (including host objects
like DOM elements) are fronted in the debugger by `Debugger.Object`
instances, which provide reflection-oriented methods for inspecting their
referents; see `Debugger.Object`, below.

Of the debugger's objects, only `Debugger.Object` instances may be passed
to the debuggee: when this occurs, the debuggee receives the
`Debugger.Object`'s referent, not the `Debugger.Object` instance itself.

In the descriptions below, the term "debuggee value" means either a
primitive value or a `Debugger.Object` instance; it is a value that might
be received from the debuggee, or that could be passed to the debuggee.


## Debuggee Code

Each `Debugger` instance maintains a set of global objects that, taken
together, comprise the debuggee. Code evaluated in the scope of a debuggee
global object, directly or indirectly, is considered *debuggee code*.
Similarly:

- a *debuggee frame* is a frame running debuggee code;

- a *debuggee function* is a function that closes over a debuggee
  global object (and thus the function's code is debuggee code);

- a *debuggee environment* is an environment whose outermost
  enclosing environment is a debuggee global object; and

- a *debuggee script* is a script containing debuggee code.


## Completion Values

When a debuggee stack frame completes its execution, or when some sort
of debuggee call initiated by the debugger finishes, the `Debugger`
interface provides a value describing how the code completed; these are
called *completion values*. A completion value has one of the
following forms:

<code>{ return: <i>value</i> }</code>
:   The code completed normally, returning <i>value</i>. <i>Value</i> is a
    debuggee value.

<code>{ yield: <i>value</i> }</code>
:   <i>(Not yet implemented.)</i> The running code is a generator frame
    which has yielded <i>value</i>. <i>Value</i> is a debuggee value.

<code>{ throw: <i>value</i> }</code>
:   The code threw <i>value</i> as an exception. <i>Value</i> is a debuggee
    value.

`null`
:   The code was terminated, as if by the "slow script" dialog box.

If control reaches the end of a generator frame, the completion value is
<code>{ throw: <i>stop</i> }</code> where <i>stop</i> is a
`Debugger.Object` object representing the `StopIteration` object being
thrown.


## Resumption Values

As the debuggee runs, the `Debugger` interface calls various
debugger-provided handler functions to report the debuggee's behavior.
Some of these calls can return a value indicating how the debuggee's
execution should continue; these are called *resumption values*. A
resumption value has one of the following forms:

`undefined`
:   The debuggee should continue execution normally.

<code>{ return: <i>value</i> }</code>
:   Return <i>value</i> immediately as the current value of the function.
    <i>Value</i> must be a debuggee value. (Most handler functions support
    this, except those whose descriptions say otherwise.) If the function
    was called as a constructor (that is, via a `new` expression), then
    <i>value</i> serves as the value returned by the function's body, not
    that produced by the `new` expression: if the value is not an object,
    the `new` expression returns the frame's `this` value.

<code>{ yield: <i>value</i> }</code>
:   <i>(Not yet implemented.)</i> Yield <i>value</i> immediately as the
    next value of the current frame, which must be a generator frame.
    <i>Value</i> is a debuggee value. The current frame must be a generator
    frame that has not yet completed in some other way. You may use `yield`
    resumption values to substitute a new value or one already yielded by a
    generator, or to make a generator yield additional values.

<code>{ throw: <i>value</i> }</code>
:   Throw <i>value</i> as an exception from the current bytecode
    instruction. <i>Value</i> must be a debuggee value.

`null`
:   Terminate the debuggee, as if it had been cancelled by the "slow script"
    dialog box.

If a function that would normally return a resumption value to indicate
how the debuggee should continue instead throws an exception, we never
propagate such an exception to the debuggee; instead, we call the
associated `Debugger` instance's `uncaughtExceptionHook` property, as
described below.


## The `Debugger.DebuggeeWouldRun` Exception

Some debugger operations that appear to simply inspect the debuggee's state
may actually cause debuggee code to run. For example, reading a variable
might run a getter function on the global or on a `with` expression's
operand; and getting an object's property descriptor will run a handler
trap if the object is a proxy. To protect the debugger's integrity, only
methods whose stated purpose is to run debuggee code can do so. These
methods are called [invocation functions][inv fr], and they follow certain
common conventions to report the debuggee's behavior safely. For other
methods, if their normal operation would cause debuggee code to run, they
throw an instance of the `Debugger.DebuggeeWouldRun` exception.

A `Debugger.DebuggeeWouldRun` exception may have a `cause` property,
providing more detailed information on why the debuggee would have run. The
`cause` property's value is one of the following strings:

  <i>cause</i> value   meaning
  -------------------- --------------------------------------------------------------------------------
  "proxy"              Carrying out the operation would have caused a proxy handler to run.
  "getter"             Carrying out the operation would have caused an object property getter to run.
  "setter"             Carrying out the operation would have caused an object property setter to run.

If the system can't determine why control attempted to enter the debuggee,
it will leave the exception's `cause` property undefined.
