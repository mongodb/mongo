# TCMalloc Compatibility Guidelines

This document details what we expect from well-behaved users. Any usage of
TCMalloc libraries outside of these technical boundaries may result in breakage
when upgrading to newer versions of TCMalloc.

Put another way: don't do things that make TCMalloc API maintenance tasks
harder. If you misuse TCMalloc APIs, you're on your own.

Additionally, because TCMalloc depends on Abseil, Abseil's
[compatibility guidelines](https://abseil.io/about/compatibility) also apply.

## What Users Must (And Must Not) Do

*   **Do not depend on a compiled representation of TCMalloc.** We do not
    promise any ABI compatibility &mdash; we intend for TCMalloc to be built
    from source, hopefully from head. The internal layout of our types may
    change at any point, without notice. Building TCMalloc in the presence of
    different C++ standard library types may change Abseil types, especially for
    pre-adopted types (`string_view`, `variant`, etc) &mdash; these will become
    typedefs and their ABI will change accordingly.
*   **Do not rely on dynamic loading/unloading.** TCMalloc does not support
    dynamic loading and unloading.
*   **You may not open namespace `tcmalloc`.** You are not allowed to define
    additional names in namespace `tcmalloc`, nor are you allowed to specialize
    anything we provide.
*   **You may not depend on the signatures of TCMalloc APIs.** You cannot take
    the address of APIs in TCMalloc (that would prevent us from adding overloads
    without breaking you). You cannot use metaprogramming tricks to depend on
    those signatures either. (This is also similar to the restrictions in the
    C++ standard.)
*   **You may not forward declare TCMalloc APIs.** This is actually a sub-point
    of "do not depend on the signatures of TCMalloc APIs" as well as "do not
    open namespace `tcmalloc`", but can be surprising. Any refactoring that
    changes template parameters, default parameters, or namespaces will be a
    breaking change in the face of forward-declarations.
*   **Do not depend upon internal details.** This should go without saying: if
    something is in a namespace or filename/path that includes the word
    "internal", you are not allowed to depend upon it. It's an implementation
    detail. You cannot friend it, you cannot include it, you cannot mention it
    or refer to it in any way.
*   **Include What You Use.** We may make changes to the internal `#include`
    graph for TCMalloc headers - if you use an API, please include the relevant
    header file directly.
