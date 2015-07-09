# Debugger.Environment

A `Debugger.Environment` instance represents a lexical environment,
associating names with variables. Each [`Debugger.Frame`][frame] instance
representing a debuggee frame has an associated environment object
describing the variables in scope in that frame; and each
[`Debugger.Object`][object] instance representing a debuggee function has an
environment object representing the environment the function has closed
over.

ECMAScript environments form a tree, in which each local environment is
parented by its enclosing environment (in ECMAScript terms, its 'outer'
environment). We say an environment <i>binds</i> an identifier if that
environment itself associates the identifier with a variable, independently
of its outer environments. We say an identifier is <i>in scope</i> in an
environment if the identifier is bound in that environment or any enclosing
environment.

SpiderMonkey creates `Debugger.Environment` instances as needed as the
debugger inspects stack frames and function objects; calling
`Debugger.Environment` as a function or constructor raises a `TypeError`
exception.

SpiderMonkey creates exactly one `Debugger.Environment` instance for each
environment it presents via a given [`Debugger`][debugger-object] instance:
if the debugger encounters the same environment through two different
routes (perhaps two functions have closed over the same environment),
SpiderMonkey presents the same `Debugger.Environment` instance to the
debugger each time. This means that the debugger can use the `==` operator
to recognize when two `Debugger.Environment` instances refer to the same
environment in the debuggee, and place its own properties on a
`Debugger.Environment` instance to store metadata about particular
environments.

(If more than one [`Debugger`][debugger-object] instance is debugging the
same code, each [`Debugger`][debugger-object] gets a separate
`Debugger.Environment` instance for a given environment. This allows the
code using each [`Debugger`][debugger-object] instance to place whatever
properties it likes on its own [`Debugger.Object`][object] instances,
without worrying about interfering with other debuggers.)

If a `Debugger.Environment` instance's referent is not a debuggee
environment, then attempting to access its properties (other than
`inspectable`) or call any its methods throws an instance of `Error`.

`Debugger.Environment` instances protect their referents from the
garbage collector; as long as the `Debugger.Environment` instance is
live, the referent remains live. Garbage collection has no visible
effect on `Debugger.Environment` instances.


## Accessor Properties of the Debugger.Environment Prototype Object

A `Debugger.Environment` instance inherits the following accessor
properties from its prototype:

`inspectable`
:   True if this environment is a debuggee environment, and can therefore
    be inspected. False otherwise. All other properties and methods of
    `Debugger.Environment` instances throw if applied to a non-inspectable
    environment.

`type`
:   The type of this environment object, one of the following values:

    * "declarative", indicating that the environment is a declarative
      environment record. Function calls, calls to `eval`, `let` blocks,
      `catch` blocks, and the like create declarative environment records.

    * "object", indicating that the environment's bindings are the
      properties of an object. The global object and DOM elements appear in
      the chain of environments via object environments. (Note that `with`
      statements have their own environment type.)

    * "with", indicating that the environment was introduced by a `with`
      statement.

`parent`
:   The environment that encloses this one (the "outer" environment, in
    ECMAScript terminology), or `null` if this is the outermost environment.

`object`
:   A [`Debugger.Object`][object] instance referring to the object whose
    properties this environment reflects. If this is a declarative
    environment record, this accessor throws a `TypeError` (since
    declarative environment records have no such object). Both `"object"`
    and `"with"` environments have `object` properties that provide the
    object whose properties they reflect as variable bindings.

`callee`
:   If this environment represents the variable environment (the top-level
    environment within the function, which receives `var` definitions) for
    a call to a function <i>f</i>, then this property's value is a
    [`Debugger.Object`][object] instance referring to <i>f</i>. Otherwise,
    this property's value is `null`.

`optimizedOut`
:   True if this environment is optimized out. False otherwise. For example,
    functions whose locals are never aliased may present optimized-out
    environments. When true, `getVariable` returns an ordinary JavaScript
    object whose `optimizedOut` property is true on all bindings, and
    `setVariable` throws a `ReferenceError`.


## Function Properties of the Debugger.Environment Prototype Object

The methods described below may only be called with a `this` value
referring to a `Debugger.Environment` instance; they may not be used as
methods of other kinds of objects.

`names()`
:   Return an array of strings giving the names of the identifiers bound by
    this environment. The result does not include the names of identifiers
    bound by enclosing environments.

<code>getVariable(<i>name</i>)</code>
:   Return the value of the variable bound to <i>name</i> in this
    environment, or `undefined` if this environment does not bind
    <i>name</i>. <i>Name</i> must be a string that is a valid ECMAScript
    identifier name. The result is a debuggee value.

    JavaScript engines often omit variables from environments, to save space
    and reduce execution time. If the given variable should be in scope, but
    `getVariable` is unable to produce its value, it returns an ordinary
    JavaScript object (not a [`Debugger.Object`][object] instance) whose
    `optimizedOut` property is `true`.

    This is not an [invocation function][inv fr];
    if this call would cause debuggee code to run (say, because the
    environment is a `"with"` environment, and <i>name</i> refers to an
    accessor property of the `with` statement's operand), this call throws a
    [`Debugger.DebuggeeWouldRun`][wouldrun]
    exception.

<code>setVariable(<i>name</i>, <i>value</i>)</code>
:   Store <i>value</i> as the value of the variable bound to <i>name</i> in
    this environment. <i>Name</i> must be a string that is a valid
    ECMAScript identifier name; <i>value</i> must be a debuggee value.

    If this environment binds no variable named <i>name</i>, throw a
    `ReferenceError`.

    This is not an [invocation function][inv fr];
    if this call would cause debuggee code to run, this call throws a
    [`Debugger.DebuggeeWouldRun`][wouldrun]
    exception.

<code>getVariableDescriptor(<i>name</i>)</code>
:   Return an property descriptor describing the variable bound to
    <i>name</i> in this environment, of the sort returned by
    `Debugger.Object.prototype.getOwnPropertyDescriptor`. <i>Name</i> must
    be a string whose value is a valid ECMAScript identifier name.

    If this is an `"object"` or `"with"` environment record, this simply
    returns the descriptor for the given property of the environment's
    object. If this is a declarative environment record, this returns a
    descriptor reflecting the binding's mutability as the descriptor's
    `writable` property, and its deletability as the descriptor's
    `configurable` property. All declarative environment record bindings are
    marked as `enumerable`. <i>(This isn't great; the semantics of variables
    in declarative enviroments don't really match those of properties, so
    writing code that operates properly on descriptors for either kind may
    be difficult.)</i>

    If this environment binds no variable named <i>name</i>, throw a
    `ReferenceError`.

<code>defineVariable(<i>name</i>, <i>descriptor</i>)</code>
:   Create or reconfigure the variable bound to <i>name</i> in this
    environment according to <i>descriptor</i>. <i>Descriptor</i> is the
    sort of value returned by `getVariableDescriptor`. On success, return
    `undefined`; on failure, throw an appropriate exception. <i>Name</i>
    must be a string whose value is a valid ECMAScript identifier name.

    If implementation restrictions prevent SpiderMonkey from creating or
    reconfiguring the variable as requested, this call throws an `Error`
    exception.

<code>deleteVariable(<i>name</i>)</code>
:   Delete this environment's binding for <i>name</i>.

    If this environment binds no variable named <i>name</i>, throw a
    `ReferenceError`.

    If implementation restrictions prevent SpiderMonkey from deleting the
    variable as requested, this call throws an `Error` exception.

<code>find(<i>name</i>)</code>
:   Return a reference to the innermost environment, starting with this
    environment, that binds <i>name</i>. If <i>name</i> is not in scope in
    this environment, return `null`. <i>Name</i> must be a string whose
    value is a valid ECMAScript identifier name.

<code>eval(<i>code</i>)</code> <i>(future plan)</i>
:   Evaluate <i>code</i> in this environment, and return a
    [completion value][cv] describing how it completed. <i>Code</i> is a
    string. All extant handler methods, breakpoints, watchpoints, and so on
    remain active during the call. This function follows the
    [invocation function conventions][inv fr].

    <i>Code</i> is interpreted as strict mode code when it contains a Use
    Strict Directive.

    If <i>code</i> is not strict mode code, then variable declarations in
    <i>code</i> affect this environment. (In the terms used by the
    ECMAScript specification, the `VariableEnvironment` of the execution
    context for the eval code is the `VariableEnvironment` this
    `Debugger.Environment` instance represents.) If implementation
    restrictions prevent SpiderMonkey from extending this environment as
    requested, this call throws an `Error` exception.

<code>evalWithBindings(<i>code</i>, <i>bindings</i>)</code> <i>(future plan)</i>
:   Like `eval`, but evaluate <i>code</i> in this environment, extended with
    bindings from the object <i>bindings</i>. For each own enumerable
    property of <i>bindings</i> named <i>name</i> whose value is
    <i>value</i>, include a variable in the environment in which <i>code</i>
    is evaluated named <i>name</i>, whose value is <i>value</i>. Each
    <i>value</i> must be a debuggee value. (This is not like a `with`
    statement: <i>code</i> may access, assign to, and delete the introduced
    bindings without having any effect on the <i>bindings</i> object.)

    This method allows debugger code to introduce temporary bindings that
    are visible to the given debuggee code and which refer to debugger-held
    debuggee values, and do so without mutating any existing debuggee
    environment.

    Note that, like `eval`, declarations in the <i>code</i> passed to
    `evalWithBindings` affect this environment, even as <i>code</i> is
    evaluated with <i>bindings</i> visible. (In the terms used by the
    ECMAScript specification, the `VariableEnvironment` of the execution
    context for the eval code is the `VariableEnvironment` this environment
    represents, and the <i>bindings</i> appear in a new declarative
    environment, which is the eval code's `LexicalEnvironment`.) If
    implementation restrictions prevent SpiderMonkey from extending this
    environment as requested, this call throws an `Error` exception.


