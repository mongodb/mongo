# Debugger.Object

A `Debugger.Object` instance represents an object in the debuggee,
providing reflection-oriented methods to inspect and modify its referent.
The referent's properties do not appear directly as properties of the
`Debugger.Object` instance; the debugger can access them only through
methods like `Debugger.Object.prototype.getOwnPropertyDescriptor` and
`Debugger.Object.prototype.defineProperty`, ensuring that the debugger will
not inadvertently invoke the referent's getters and setters.

SpiderMonkey creates exactly one `Debugger.Object` instance for each
debuggee object it presents to a given [`Debugger`][debugger-object]
instance: if the debugger encounters the same object through two different
routes (perhaps two functions are called on the same object), SpiderMonkey
presents the same `Debugger.Object` instance to the debugger each time.
This means that the debugger can use the `==` operator to recognize when
two `Debugger.Object` instances refer to the same debuggee object, and
place its own properties on a `Debugger.Object` instance to store metadata
about particular debuggee objects.

JavaScript code in different compartments can have different views of the
same object. For example, in Firefox, code in privileged compartments sees
content DOM element objects without redefinitions or extensions made to
that object's properties by content code. (In Firefox terminology,
privileged code sees the element through an "xray wrapper".) To ensure that
debugger code sees each object just as the debuggee would, each
`Debugger.Object` instance presents its referent as it would be seen from a
particular compartment. This "viewing compartment" is chosen to match the
way the debugger came across the referent. As a consequence, a single
[`Debugger`][debugger-object] instance may actually have several
`Debugger.Object` instances: one for each compartment from which the
referent is viewed.

If more than one [`Debugger`][debugger-object] instance is debugging the
same code, each [`Debugger`][debugger-object] gets a separate
`Debugger.Object` instance for a given object. This allows the code using
each [`Debugger`][debugger-object] instance to place whatever properties it
likes on its own `Debugger.Object` instances, without worrying about
interfering with other debuggers.

While most `Debugger.Object` instances are created by SpiderMonkey in the
process of exposing debuggee's behavior and state to the debugger, the
debugger can use `Debugger.Object.prototype.makeDebuggeeValue` to create
`Debugger.Object` instances for given debuggee objects, or use
`Debugger.Object.prototype.copy` and `Debugger.Object.prototype.create` to
create new objects in debuggee compartments, allocated as if by particular
debuggee globals.

`Debugger.Object` instances protect their referents from the garbage
collector; as long as the `Debugger.Object` instance is live, the referent
remains live. This means that garbage collection has no visible effect on
`Debugger.Object` instances.


## Accessor Properties of the Debugger.Object prototype

A `Debugger.Object` instance inherits the following accessor properties
from its prototype:

`proto`
:   The referent's prototype (as a new `Debugger.Object` instance), or
    `null` if it has no prototype.

`class`
:   A string naming the ECMAScript `[[Class]]` of the referent.

`callable`
:   `true` if the referent is a callable object (such as a function or a
    function proxy); false otherwise.

`name`
:   The name of the referent, if it is a named function. If the referent is
    an anonymous function, or not a function at all, this is `undefined`.

    This accessor returns whatever name appeared after the `function`
    keyword in the source code, regardless of whether the function is the
    result of instantiating a function declaration (which binds the
    function to its name in the enclosing scope) or evaluating a function
    expression (which binds the function to its name only within the
    function's body).

`displayName`
:   The referent's display name, if the referent is a function with a
    display name. If the referent is not a function, or if it has no display
    name, this is `undefined`.

    If a function has a given name, its display name is the same as its
    given name. In this case, the `displayName` and `name` properties are
    equal.

    If a function has no name, SpiderMonkey attempts to infer an appropriate
    name for it given its context. For example:

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

`parameterNames`
:   If the referent is a debuggee function, the names of the its parameters,
    as an array of strings. If the referent is not a debuggee function, or
    not a function at all, this is `undefined`.

    If the referent is a host function for which parameter names are not
    available, return an array with one element per parameter, each of which
    is `undefined`.

    If the referent is a function proxy, return an empty array.

    If the referent uses destructuring parameters, then the array's elements
    reflect the structure of the parameters. For example, if the referent is
    a function declared in this way:

    ```language-js
    function f(a, [b, c], {d, e:f}) { ... }
    ```

    then this `Debugger.Object` instance's `parameterNames` property would
    have the value:

    ```language-js
    ["a", ["b", "c"], {d:"d", e:"f"}]
    ```

`script`
:   If the referent is a function that is debuggee code, this is that
    function's script, as a [`Debugger.Script`][script] instance. If the
    referent is a function proxy or not debuggee code, this is `undefined`.

`environment`
:   If the referent is a function that is debuggee code, a
    [`Debugger.Environment`][environment] instance representing the lexical
    environment enclosing the function when it was created. If the referent
    is a function proxy or not debuggee code, this is `undefined`.

`isBoundFunction`
:   `true` if the referent is a bound function; `false` otherwise.

`isArrowFunction`
:   `true` if the referent is an arrow function; `false` otherwise.

`boundTargetFunction`
:   If the referent is a bound function, this is its target function—the
    function that was bound to a particular `this` object. If the referent
    is not a bound function, this is `undefined`.

`boundThis`
:   If the referent is a bound function, this is the `this` value it was
    bound to. If the referent is not a bound function, this is `undefined`.

`boundArguments`
:   If the referent is a bound function, this is an array (in the Debugger
    object's compartment) that contains the debuggee values of the `arguments`
    object it was bound to. If the referent is not a bound function, this is
    `undefined`.

`proxyHandler`
:   If the referent is a proxy whose handler object was allocated by
    debuggee code, this is its handler object—the object whose methods are
    invoked to implement accesses of the proxy's properties. If the referent
    is not a proxy whose handler object was allocated by debuggee code, this
    is `null`.

`proxyCallTrap`
:   If the referent is a function proxy whose handler object was allocated
    by debuggee code, this is its call trap function—the function called
    when the function proxy is called. If the referent is not a function
    proxy whose handler object was allocated by debuggee code, this is
    `null`.

`proxyConstructTrap`
:   If the referent is a function proxy whose handler object was allocated
    by debuggee code, its construction trap function—the function called
    when the function proxy is called via a `new` expression. If the
    referent is not a function proxy whose handler object was allocated by
    debuggee code, this is `null`.

`global`
:   A `Debugger.Object` instance referring to the global object in whose
    scope the referent was allocated. This does not unwrap cross-compartment
    wrappers: if the referent is a wrapper, the result refers to the
    wrapper's global, not the wrapped object's global. The result refers to
    the global directly, not via a wrapper.

<code id="allocationsite">allocationSite</code>
:   If [object allocation site tracking][tracking-allocs] was enabled when this
    `Debugger.Object`'s referent was allocated, return the
    [JavaScript execution stack][saved-frame] captured at the time of the
    allocation. Otherwise, return `null`.


## Function Properties of the Debugger.Object prototype

The functions described below may only be called with a `this` value
referring to a `Debugger.Object` instance; they may not be used as methods
of other kinds of objects. The descriptions use "referent" to mean "the
referent of this `Debugger.Object` instance".

Unless otherwise specified, these methods are not
[invocation functions][inv fr]; if a call would cause debuggee code to run
(say, because it gets or sets an accessor property whose handler is
debuggee code, or because the referent is a proxy whose traps are debuggee
code), the call throws a [`Debugger.DebuggeeWouldRun`][wouldrun] exception.

<code>getProperty(<i>name</i>)</code>
:   Return the value of the referent's property named <i>name</i>, or
    `undefined` if it has no such property. <i>Name</i> must be a string.
    The result is a debuggee value.

<code>setProperty(<i>name</i>, <i>value</i>)</code>
:   Store <i>value</i> as the value of the referent's property named
    <i>name</i>, creating the property if it does not exist. <i>Name</i>
    must be a string; <i>value</i> must be a debuggee value.

<code>getOwnPropertyDescriptor(<i>name</i>)</code>
:   Return a property descriptor for the property named <i>name</i> of the
    referent. If the referent has no such property, return `undefined`.
    (This function behaves like the standard
    `Object.getOwnPropertyDescriptor` function, except that the object being
    inspected is implicit; the property descriptor returned is allocated as
    if by code scoped to the debugger's global object (and is thus in the
    debugger's compartment); and its `value`, `get`, and `set` properties,
    if present, are debuggee values.)

`getOwnPropertyNames()`
:   Return an array of strings naming all the referent's own properties, as
    if <code>Object.getOwnPropertyNames(<i>referent</i>)</code> had been
    called in the debuggee, and the result copied in the scope of the
    debugger's global object.

<code>defineProperty(<i>name</i>, <i>attributes</i>)</code>
:   Define a property on the referent named <i>name</i>, as described by
    the property descriptor <i>descriptor</i>. Any `value`, `get`, and
    `set` properties of <i>attributes</i> must be debuggee values. (This
    function behaves like `Object.defineProperty`, except that the target
    object is implicit, and in a different compartment from the function
    and descriptor.)

<code>defineProperties(<i>properties</i>)</code>
:   Add the properties given by <i>properties</i> to the referent. (This
    function behaves like `Object.defineProperties`, except that the target
    object is implicit, and in a different compartment from the
    <i>properties</i> argument.)

<code>deleteProperty(<i>name</i>)</code>
:   Remove the referent's property named <i>name</i>. Return true if the
    property was successfully removed, or if the referent has no such
    property. Return false if the property is non-configurable.

`seal()`
:   Prevent properties from being added to or deleted from the referent.
    Return this `Debugger.Object` instance. (This function behaves like the
    standard `Object.seal` function, except that the object to be sealed is
    implicit and in a different compartment from the caller.)

`freeze()`
:   Prevent properties from being added to or deleted from the referent, and
    mark each property as non-writable. Return this `Debugger.Object`
    instance. (This function behaves like the standard `Object.freeze`
    function, except that the object to be sealed is implicit and in a
    different compartment from the caller.)

`preventExtensions()`
:   Prevent properties from being added to the referent. (This function
    behaves like the standard `Object.preventExtensions` function, except
    that the object to operate on is implicit and in a different compartment
    from the caller.)

`isSealed()`
:   Return true if the referent is sealed—that is, if it is not extensible,
    and all its properties have been marked as non-configurable. (This
    function behaves like the standard `Object.isSealed` function, except
    that the object inspected is implicit and in a different compartment
    from the caller.)

`isFrozen()`
:   Return true if the referent is frozen—that is, if it is not extensible,
    and all its properties have been marked as non-configurable and
    read-only. (This function behaves like the standard `Object.isFrozen`
    function, except that the object inspected is implicit and in a
    different compartment from the caller.)

`isExtensible()`
:   Return true if the referent is extensible—that is, if it can have new
    properties defined on it. (This function behaves like the standard
    `Object.isExtensible` function, except that the object inspected is
    implicit and in a different compartment from the caller.)

<code>copy(<i>value</i>)</code>
:   Apply the HTML5 "structured cloning" algorithm to create a copy of
    <i>value</i> in the referent's global object (and thus in the referent's
    compartment), and return a `Debugger.Object` instance referring to the
    copy.

    Note that this returns primitive values unchanged. This means you can
    use `Debugger.Object.prototype.copy` as a generic "debugger value to
    debuggee value" conversion function—within the limitations of the
    "structured cloning" algorithm.

<code>create(<i>prototype</i>, [<i>properties</i>])</code>
:   Create a new object in the referent's global (and thus in the
    referent's compartment), and return a `Debugger.Object` referring to
    it. The new object's prototype is <i>prototype</i>, which must be an
    `Debugger.Object` instance. The new object's properties are as given by
    <i>properties</i>, as if <i>properties</i> were passed to
    `Debugger.Object.prototype.defineProperties`, with the new
    `Debugger.Object` instance as the `this` value.

<code>makeDebuggeeValue(<i>value</i>)</code>
:   Return the debuggee value that represents <i>value</i> in the debuggee.
    If <i>value</i> is a primitive, we return it unchanged; if <i>value</i>
    is an object, we return the `Debugger.Object` instance representing
    that object, wrapped appropriately for use in this `Debugger.Object`'s
    referent's compartment.

    Note that, if <i>value</i> is an object, it need not be one allocated
    in a debuggee global, nor even a debuggee compartment; it can be any
    object the debugger wishes to use as a debuggee value.

    As described above, each `Debugger.Object` instance presents its
    referent as viewed from a particular compartment. Given a
    `Debugger.Object` instance <i>d</i> and an object <i>o</i>, the call
    <code><i>d</i>.makeDebuggeeValue(<i>o</i>)</code> returns a
    `Debugger.Object` instance that presents <i>o</i> as it would be seen
    by code in <i>d</i>'s compartment.

<code>decompile([<i>pretty</i>])</code>
:   If the referent is a function that is debuggee code, return the
    JavaScript source code for a function definition equivalent to the
    referent function in its effect and result, as a string. If
    <i>pretty</i> is present and true, produce indented code with line
    breaks. If the referent is not a function that is debuggee code, return
    `undefined`.

<code>call(<i>this</i>, <i>argument</i>, ...)</code>
:   If the referent is callable, call it with the given <i>this</i> value
    and <i>argument</i> values, and return a [completion value][cv]
    describing how the call completed. <i>This</i> should be a debuggee
    value, or `{ asConstructor: true }` to invoke the referent as a
    constructor, in which case SpiderMonkey provides an appropriate `this`
    value itself. Each <i>argument</i> must be a debuggee value. All extant
    handler methods, breakpoints, watchpoints, and so on remain active
    during the call. If the referent is not callable, throw a `TypeError`.
    This function follows the [invocation function conventions][inv fr].

<code>apply(<i>this</i>, <i>arguments</i>)</code>
:   If the referent is callable, call it with the given <i>this</i> value
    and the argument values in <i>arguments</i>, and return a
    [completion value][cv] describing how the call completed. <i>This</i>
    should be a debuggee value, or `{ asConstructor: true }` to invoke
    <i>function</i> as a constructor, in which case SpiderMonkey provides
    an appropriate `this` value itself. <i>Arguments</i> must either be an
    array (in the debugger) of debuggee values, or `null` or `undefined`,
    which are treated as an empty array. All extant handler methods,
    breakpoints, watchpoints, and so on remain active during the call. If
    the referent is not callable, throw a `TypeError`. This function
    follows the [invocation function conventions][inv fr].

<code>evalInGlobal(<i>code</i>, [<i>options</i>])</code>
:   If the referent is a global object, evaluate <i>code</i> in that global
    environment, and return a [completion value][cv] describing how it completed.
    <i>Code</i> is a string. All extant handler methods, breakpoints,
    watchpoints, and so on remain active during the call. This function
    follows the [invocation function conventions][inv fr].
    If the referent is not a global object, throw a `TypeError` exception.

    <i>Code</i> is interpreted as strict mode code when it contains a Use
    Strict Directive.

    If <i>code</i> is not strict mode code, then variable declarations in
    <i>code</i> affect the referent global object. (In the terms used by the
    ECMAScript specification, the `VariableEnvironment` of the execution
    context for the eval code is the referent.)

    The <i>options</i> argument is as for [`Debugger.Frame.prototype.eval`][fr eval].

<code>evalInGlobalWithBindings(<i>code</i>, <i>bindings</i>, [<i>options</i>])</code>
:   Like `evalInGlobal`, but evaluate <i>code</i> using the referent as the
    variable object, but with a lexical environment extended with bindings
    from the object <i>bindings</i>. For each own enumerable property of
    <i>bindings</i> named <i>name</i> whose value is <i>value</i>, include a
    variable in the lexical environment in which <i>code</i> is evaluated
    named <i>name</i>, whose value is <i>value</i>. Each <i>value</i> must
    be a debuggee value. (This is not like a `with` statement: <i>code</i>
    may access, assign to, and delete the introduced bindings without having
    any effect on the <i>bindings</i> object.)

    This method allows debugger code to introduce temporary bindings that
    are visible to the given debuggee code and which refer to debugger-held
    debuggee values, and do so without mutating any existing debuggee
    environment.

    Note that, like `evalInGlobal`, if the code passed to
    `evalInGlobalWithBindings` is not strict mode code, then any
    declarations it contains affect the referent global object, even as
    <i>code</i> is evaluated in an environment extended according to
    <i>bindings</i>. (In the terms used by the ECMAScript specification, the
    `VariableEnvironment` of the execution context for non-strict eval code
    is the referent, and the <i>bindings</i> appear in a new declarative
    environment, which is the eval code's `LexicalEnvironment`.)

    The <i>options</i> argument is as for [`Debugger.Frame.prototype.eval`][fr eval].

`asEnvironment()`
:   If the referent is a global object, return the [`Debugger.Environment`][environment]
    instance representing the referent as a variable environment for
    evaluating code. If the referent is not a global object, throw a
    `TypeError`.

<code>setObjectWatchpoint(<i>handler</i>)</code> <i>(future plan)</i>
:   Set a watchpoint on all the referent's own properties, reporting events
    by calling <i>handler</i>'s methods. Any previous watchpoint handler on
    this `Debugger.Object` instance is replaced. If <i>handler</i> is null,
    the referent is no longer watched. <i>Handler</i> may have the following
    methods, called under the given circumstances:

    <code>add(<i>frame</i>, <i>name</i>, <i>descriptor</i>)</code>
    :   A property named <i>name</i> has been added to the referent.
        <i>Descriptor</i> is a property descriptor of the sort accepted by
        `Debugger.Object.prototype.defineProperty`, giving the newly added
        property's attributes.

    <code>delete(<i>frame</i>, <i>name</i>)</code>
    :   The property named <i>name</i> is about to be deleted from the referent.

    <code>change(<i>frame</i>, <i>name</i>, <i>oldDescriptor</i>, <i>newDescriptor</i>)</code>
    :   The existing property named <i>name</i> on the referent is being changed
        from those given by <i>oldDescriptor</i> to those given by
        <i>newDescriptor</i>. This handler method is only called when attributes
        of the property other than its value are being changed; if only the
        value is changing, SpiderMonkey calls the handler's `set` method.

    <code>set(<i>frame</i>, <i>oldValue</i>, <i>newValue</i>)</code>
    :   The data property named <i>name</i> of the referent is about to have its
        value changed from <i>oldValue</i> to <i>newValue</i>.

        SpiderMonkey only calls this method on assignments to data properties
        that will succeed; assignments to un-writable data properties fail
        without notifying the debugger.

    <code>extensionsPrevented(<i>frame</i>)</code>
    :   The referent has been made non-extensible, as if by a call to
        `Object.preventExtensions`.

    For all watchpoint handler methods:

    * Handler calls receive the handler object itself as the `this` value.

    * The <i>frame</i> argument is the current stack frame, whose code is
      about to perform the operation on the object being reported.

    * If the method returns `undefined`, then SpiderMonkey makes the announced
      change to the object, and continues execution normally. If the method
      returns an object:

    * If the object has a `superseded` property whose value is a true value,
      then SpiderMonkey does not make the announced change.

    * If the object has a `resume` property, its value is taken as a
      [resumption value][rv], indicating how
      execution should proceed. (However, `return` resumption values are not
      supported.)

    * If a given method is absent from <i>handler</i>, then events of that
      sort are ignored. The watchpoint consults <i>handler</i>'s properties
      each time an event occurs, so adding methods to or removing methods from
      <i>handler</i> after setting the watchpoint enables or disables
      reporting of the corresponding events.

    * Values passed to <i>handler</i>'s methods are debuggee values.
      Descriptors passed to <i>handler</i>'s methods are ordinary objects in
      the debugger's compartment, except for `value`, `get`, and `set`
      properties in descriptors, which are debuggee values; they are the sort
      of value expected by `Debugger.Object.prototype.defineProperty`.

    * Watchpoint handler calls are cross-compartment, intra-thread calls: the
      call takes place in the same thread that changed the property, and in
      <i>handler</i>'s method's compartment (typically the same as the
      debugger's compartment).

    The new watchpoint belongs to the [`Debugger`][debugger-object] instance to which this
    `Debugger.Object` instance belongs; disabling the [`Debugger`][debugger-object] instance
    disables this watchpoint.

`clearObjectWatchpoint()` <i>(future plan)</i>
:   Remove any object watchpoint set on the referent.

<code>setPropertyWatchpoint(<i>name</i>, <i>handler</i>)</code> <i>(future plan)</i>
:   Set a watchpoint on the referent's property named <i>name</i>, reporting
    events by calling <i>handler</i>'s methods. Any previous watchpoint
    handler on this property for this `Debugger.Object` instance is
    replaced. If <i>handler</i> is null, the property is no longer watched.
    <i>Handler</i> is as described for
    `Debugger.Object.prototype.setObjectWatchpoint`, except that it does not
    receive `extensionsPrevented` events.

<code>clearPropertyWatchpoint(<i>name</i>)</code> <i>(future plan)</i>
:   Remove any watchpoint set on the referent's property named <i>name</i>.

`unwrap()`
:   If the referent is a wrapper that this `Debugger.Object`'s compartment
    is permitted to unwrap, return a `Debugger.Object` instance referring to
    the wrapped object. If we are not permitted to unwrap the referent,
    return `null`. If the referent is not a wrapper, return this
    `Debugger.Object` instance unchanged.

`unsafeDereference()`
:   Return the referent of this `Debugger.Object` instance.

    If the referent is an inner object (say, an HTML5 `Window` object),
    return the corresponding outer object (say, the HTML5 `WindowProxy`
    object). This makes `unsafeDereference` more useful in producing values
    appropriate for direct use by debuggee code, without using [invocation functions][inv fr].

    This method pierces the membrane of `Debugger.Object` instances meant to
    protect debugger code from debuggee code, and allows debugger code to
    access debuggee objects through the standard cross-compartment wrappers,
    rather than via `Debugger.Object`'s reflection-oriented interfaces. This
    method makes it easier to gradually adapt large code bases to this
    Debugger API: adapted portions of the code can use `Debugger.Object`
    instances, but use this method to pass direct object references to code
    that has not yet been updated.
