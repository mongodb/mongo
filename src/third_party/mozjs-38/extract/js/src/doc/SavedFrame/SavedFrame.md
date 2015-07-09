# `SavedFrame`

A `SavedFrame` instance is a singly linked list of stack frames. It represents a
JavaScript call stack at a past moment of execution. Younger frames hold a
reference to the frames that invoked them. The older tails are shared across
many younger frames.

`SavedFrame` stacks should generally be captured, allocated, and live within the
compartment that is being observed or debugged. Usually this is a content
compartment.

## Capturing `SavedFrame` Stacks

### From C++

Use `JS::CaptureCurrentStack` declared in `jsapi.h`.

### From JS

Use `saveStack`, accessible via `Components.utils.getJSTestingFunction()`.

## Including and Excluding Chrome Frames

Consider the following `SavedFrame` stack. Arrows represent links from child to
parent frame, `content.js` is from a compartment with content principals, and
`chrome.js` is from a compartment with chrome principals.

    function A from content.js
                |
                V
    function B from chrome.js
                |
                V
    function C from content.js

The content compartment will ever have one view of this stack: `A -> C`.

However, a chrome compartment has a choice: it can either take the same view
that the content compartment has (`A -> C`), or it can view all stack frames,
including the frames from chrome compartments (`A -> B -> C`). To view
everything, use an `XrayWrapper`. This is the default wrapper. To see the stack
as the content compartment sees it, waive the xray wrapper with
`Components.utils.waiveXrays`:

    const contentViewOfStack = Components.utils.waiveXrays(someStack);

## Accessor Properties of the `SavedFrame.prototype` Object

`source`
:   The source URL for this stack frame, as a string.

`line`
:   The line number for this stack frame.

`column`
:   The column number for this stack frame.

`functionDisplayName`
:   Either SpiderMonkey's inferred name for this stack frame's function, or
    `null`.

`parent`
:   Either this stack frame's parent stack frame (the next older frame), or
    `null` if this is the oldest frame in the captured stack.


## Function Properties of the `SavedFrame.prototype` Object

`toString`
:   Return this frame and its parents formatted as a human readable stack trace
    string.
