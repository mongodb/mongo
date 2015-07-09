Debugger.Memory
===============

The [`Debugger API`][debugger] can help tools observe the debuggee's memory use
in various ways:

- It can mark each new object with the JavaScript call stack at which it was
  allocated.

- It can log all object allocations, yielding a stream of JavaScript call stacks
  at which allocations have occurred.

- It can compute a *census* of items belonging to the debuggee, categorizing
  items in various ways, and yielding item counts.

If <i>dbg</i> is a [`Debugger`][debugger-object] instance, then the methods and
accessor properties of <code><i>dbg</i>.memory</code> control how <i>dbg</i>
observes its debuggees' memory use. The <code><i>dbg</i>.memory</code> object is
an instance of `Debugger.Memory`; its inherited accesors and methods are
described below.


### Allocation Site Tracking

The JavaScript engine marks each new object with the call stack at which it was
allocated, if:

- the object is allocated in the scope of a global object that is a debuggee of
  some [`Debugger`][debugger-object] instance <i>dbg</i>; and

- <code><i>dbg</i>.memory.[trackingAllocationSites][tracking-allocs]</code> is
  set to `true`.

Given a [`Debugger.Object`][object] instance <i>dobj</i> referring to some
object, <code><i>dobj</i>.[allocationSite][allocation-site]</code> returns a
[saved call stack][saved-frame] indicating where <i>dobj</i>'s referent was
allocated.


### Allocation Logging

If <i>dbg</i> is a [`Debugger`][debugger-object] instance, and
<code><i>dbg</i>.memory.[trackingAllocationSites][tracking-allocs]</code> is set
to `true`, then the JavaScript engine logs each object allocated by <i>dbg</i>'s
debuggee code. You can retrieve the current log by calling
<code><i>dbg</i>.memory.[drainAllocationsLog][drain-alloc-log]</code>. You can
control the limit on the log's size by setting
<code><i>dbg</i>.memory.[maxAllocationsLogLength][max-alloc-log]</code>.


### Censuses

A *census* is a complete traversal of the graph of all reachable memory items
belonging to a particular `Debugger`'s debuggees. It produces a count of those
items, broken down by various criteria. If <i>dbg</i> is a
[`Debugger`][debugger-object] instance, you can call
<code><i>dbg</i>.memory.[takeCensus][take-census]</code> to conduct a census of
its debuggees' possessions.


Accessor Properties of the `Debugger.Memory.prototype` Object
-------------------------------------------------------------

If <i>dbg</i> is a [`Debugger`][debugger-object] instance, then
`<i>dbg</i>.memory` is a `Debugger.Memory` instance, which inherits the
following accessor properties from its prototype:

<code id='trackingallocationsites'>trackingAllocationSites</code>
:   A boolean value indicating whether this `Debugger.Memory` instance is
    capturing the JavaScript execution stack when each Object is allocated. This
    accessor property has both a getter and setter: assigning to it enables or
    disables the allocation site tracking. Reading the accessor produces `true`
    if the Debugger is capturing stacks for Object allocations, and `false`
    otherwise. Allocation site tracking is initially disabled in a new Debugger.

    Assignment is fallible: if the Debugger cannot track allocation sites, it
    throws an `Error` instance.

    You can retrieve the allocation site for a given object with the
    [`Debugger.Object.prototype.allocationSite`][allocation-site] accessor
    property.

<code id='max-alloc-log'>maxAllocationsLogLength</code>
:   The maximum number of allocation sites to accumulate in the allocations log
    at a time. This accessor can be both fetched and stored to. Its default
    value is `5000`.

<code id='allocationsLogOverflowed'>allocationsLogOverflowed</a>
:   Returns `true` if there have been more than
    [`maxAllocationsLogLength`][#max-alloc-log] allocations since the last time
    [`drainAllocationsLog`][#drain-alloc-log] was called and some data has been
    lost. Returns `false` otherwise.

Function Properties of the `Debugger.Memory.prototype` Object
-------------------------------------------------------------

<code id='drain-alloc-log'>drainAllocationsLog()</code>
:   When `trackingAllocationSites` is `true`, this method returns an array of
    recent `Object` allocations within the set of debuggees. *Recent* is
    defined as the `maxAllocationsLogLength` most recent `Object` allocations
    since the last call to `drainAllocationsLog`. Therefore, calling this
    method effectively clears the log.

    Objects in the array are of the form:

    <pre class='language-js'><code>
    {
      "timestamp": <i>timestamp</i>,
      "frame": <i>allocationSite</i>
    }
    </code></pre>

    Here <i>timestamp</i> is the timestamp of the event in units of
    microseconds since the epoch and <i>allocationSite</i> is an
    allocation site (as a [captured stack][saved-frame]).
    <i>allocationSite</i> is `null` for objects allocated with no
    JavaScript frames on the stack.

    When `trackingAllocationSites` is `false`, `drainAllocationsLog()` throws an
    `Error`.

<code id='take-census'>takeCensus()</code>
:   Carry out a census of the debuggee compartments' contents. A *census* is a
    complete traversal of the graph of all reachable memory items belonging to a
    particular `Debugger`'s debuggees. The census produces a count of those
    items, broken down by various criteria.

    The `takeCensus` method returns an object of the form:

    <pre class='language-js'><code>
    {
      "objects": { <i>class</i>: <i>tally</i>, ... },
      "scripts": <i>tally</i>,
      "strings": <i>tally</i>,
      "other": { <i>type name</i>: <i>tally</i>, ... }
    }
    </code></pre>

    Each <i>tally</i> has the form:

    <pre class='language-js'><code>
    { "count": <i>count</i> }
    </code></pre>

    where <i>count</i> is the number of items in the category.

    The `"objects"` property's value contains the tallies of JavaScript objects,
    broken down by their ECMAScript `[[Class]]` internal property values. Each
    <i>class</i> is a string.

    The `"scripts"` property's value tallies the in-memory representation of
    JavaScript code.

    The `"strings"` property's value tallies the debuggee's strings.

    The `"other"` property's value contains the tallies of other items used
    internally by SpiderMonkey, broken down by their C++ type name.

    Because performing a census requires traversing the entire graph of objects
    in debuggee compartments, it is an expensive operation. On developer
    hardware in 2014, traversing a memory graph containing roughly 130,000 nodes
    and 410,000 edges took roughly 100ms. The traversal itself temporarily
    allocates one hash table entry per node (roughly two address-sized words) in
    addition to the per-category counts, whose size depends on the number of
    categories.


Memory Use Analysis Exposes Implementation Details
--------------------------------------------------

Memory analysis may yield surprising results, because browser implementation
details that are transparent to content JavaScript often have visible effects on
memory consumption. Web developers need to know their pages' actual memory
consumption on real browsers, so it is correct for the tool to expose these
behaviors, as long as it is done in a way that helps developers make decisions
about their own code.

This section covers some areas where Firefox's actual behavior deviates from
what one might expect from the specified behavior of the web platform.


### Objects

SpiderMonkey objects usually use less memory than the naïve "table of properties
with attributes" model would suggest. For example, it is typical for many
objects to have identical sets of properties, with only the properties' values
varying from one object to the next. To take advantage of this regularity,
SpiderMonkey objects with identical sets of properties may share their property
metadata; only property values are stored directly in the object.

Array objects may also be optimized, if the set of live indices is dense.


### Strings

SpiderMonkey has three representations of strings:

- Normal: the string's text is counted in its size.

- Substring: the string is a substring of some other string, and points to that
  string for its storage. This representation may result in a small string
  retaining a very large string. However, the memory consumed by the string
  itself is a small constant independent of its size, since it is simply a
  reference to the larger string, a start position, and a length.

- Concatenations: When asked to concatenate two strings, SpiderMonkey may elect
  to delay copying the strings' data, and represent the result simply as a
  pointer to the two original strings. Again, such a string retains other
  strings, but the memory consumed by the string itself is a small constant
  independent of its size, since it is simply a pair of pointers.

SpiderMonkey converts strings from the more complex representations to the
simpler ones when it pleases. Such conversions usually increase memory
consumption.

SpiderMonkey shares some strings amongst all web pages and browser JS. These
shared strings, called *atoms*, are not included in censuses' string tallies.


### Scripts

SpiderMonkey has a complex, hybrid representation of JavaScript code. There
are four representations kept in memory:

- _Source code_. SpiderMonkey retains a copy of most JavaScript source code.

- _Compressed source code_. SpiderMonkey compresses JavaScript source code,
  and de-compresses it on demand. Heuristics determine how long to retain the
  uncompressed code.

- _Bytecode_. This is SpiderMonkey's parsed representation of JavaScript.
  Bytecode can be interpreted directly, or used as input to a just-in-time
  compiler. Source is parsed into bytecode on demand; functions that are never
  called are never parsed.

- _Machine code_. SpiderMonkey includes several just-in-time compilers, each of
  which translates JavaScript source or bytecode to machine code. Heuristics
  determine which code to compile, and which compiler to use. Machine code may
  be dropped in response to memory pressure, and regenerated as needed.

Furthermore, SpiderMonkey tracks which types of values have appeared in
variables and object properties. This type information can be large.

In a census, all the various forms of JavaScript code are placed in the
`"script"` category. Type information is accounted to the `"types"` category.
