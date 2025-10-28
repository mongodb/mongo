# MongoDB Server C++ Style Guide

This document describes common conventions used in the MongoDB server codebase.
The document is about C++, but there are a few places where JavaScript style is
discussed as well.

A firmly established style guide can make source files unsurprising as they are
more easily navigable and regular in shape.

Style rules can eliminate wasted time on minor issues in code reviews. An author
should endeavor to be style-compliant before sending a pull request for review.
This should accelerate code reviews and establish consistent expectations on code.

The guide is carefully considered by very experienced C++ engineers. C++ code
can be complex, and there are subtle correctness and maintainability risks that
can arise from certain antipatterns addressed by the guide. Style adherence
enables code authors and their reviewers to productively write safer code
without having to first rediscover those problems for themselves.

## Feedback (MongoDB internal)

This is maintained by the Server Programmability team.

- Use `#server-programmability` on Slack for discussion and clarifications.
  Contributors outside of MongoDB can use Jira instead.
- For change proposals, please feel free to add entries to the
  MongoDB C++ Style Guide Proposals document pinned to that channel.
- Jira and PRs are fine for small fixes unrelated to C++ style, such as
  typos, formatting, phrasing, and comments.

## Style

## Names of Identifiers

There's some truth in the old joke that naming is the hardest problem in
programming. It's impossible to write catch-all rules for naming, but we can set
guidelines with the intention of avoiding friction in reviews and having some
expectation of general consistency across our codebase.

- Types use `TitleCase`. First letter of each word is uppercase. Following
  letters are lowercase.

- Functions and variables use `camelCase`. First letter of each word after the
  first is uppercase. The first letter of each word, except the first, is
  uppercase.

- Namespaces use `snake_case`. No uppercase letters, and words separated by underscores.
  (See "[Namespaces](#namespaces)" section below).

- Spelling: Take care to avoid misspellings in names.
  This is more than aesthetic. It is easier on readers.
  Misspelled names can harm confidence in code quality.
  Misspelled names might be skipped by code searches.
  Our convention is to use US English spelling.

- Identifier names should be short but clear. Long sentence-like names
  become a laborious comparison exercise for readers, and can form a "wall of
  text" that can bury significant C++ keywords and operators. Local variable names
  can be particularly brief without causing confusion, provided that the enclosing
  functions remain compact and focused.

- Repetition and redundancy in names should be avoided. A function name doesn't
  need to restate the types of its arguments, for example. The arguments can
  usually speak for themselves, but explicit disambiguation may be desirable in
  some cases.

- Word abbreviations should be used carefully. When used, they should be applied
  very consistently and documented well. This keeps users from having to guess
  which words are abbreviated and which are not.

- Private members are usually named with a leading underscore (e.g. `_detail`).
  This applies to data members more consistently than to functions. Identifiers
  with a leading underscore followed by an uppercase letter are reserved by
  C++, and must not be used. Therefore, the leading `_` should not be used with
  private types and typedefs. Double underscores `__` must be avoided as well.
  See [article](https://devblogs.microsoft.com/oldnewthing/20230109-00/?p=107685).

### Constants

Constants are either ordinary variables `varName` or with a `k` as a prefix
word, like `kVarName`. You'll see both in the codebase and either is acceptable.
You may also find some older code using `MACRO_STYLE` for constants.
That should not be used in new code outside of macros.

### Test Access

Some entities are defined in an API purely to facilitate test access and
testability. We conventionally tack a `_forTest` suffix (or a `ForTest` suffix
for types) onto its name as an indicator that it should not be used by non-test
code.

## Class Definitions

While class and struct are largely equivalent in C++, this codebase uses a
convention where structs are used for simple collections of data
(possibly with methods), while classes are used for new abstractions. As a rule,
all data in a struct should be public and all data in a class should be private.
If you are unsure which to use, consider whether there are any invariants that
need to be upheld, either within or between members. If there are not, then a
struct may be appropriate.

If a type is a struct or struct-like class, then consider omitting all
constructors and letting it be a [C++ aggregate](https://en.cppreference.com/w/cpp/language/aggregate_initialization), which allows some flexibility
in initialization syntax.

If a type has invariant-preserving constructors, special behaviors, and internal
private details, it's not a `struct`. It's subjective, but structs should be a
mostly straightforward aggregation of data members.

Consider a somewhat canonical example of a `Date`, consisting of `year`,
`month`, `dayOfMonth`. The valid range of a `dayOfMonth` depends on `year` and
`month`, so this type either has an invariant, or it has to be allowed to be in
an invalid state. If the invariants of this type are enforced by the type's
constructors and setters, then it should be a `class`.

It's possible to leave such a `Date` type as a `struct` and enforce these
invariants from the outside through careful discipline among its users. This is
what C APIs have to do. We should prefer using data encapsulation and
`class` for such complex objects.

### Order of Class Members

Within a class or struct definition, try to stick to this ordering by default. A
consistent convention makes it easier for a reader to quickly understand and
navigate a class declaration.

Group public API at the top, and details at the bottom.

- `public`
- `protected`
- `private`

Within each of these visibility sections, there's a preferred order of declarations.

- Attributes of the class come first:

  - Types and type aliases, including declarations and enums
  - Static constants and static data members
  - Static functions

- Then declarations that are relevant to each instance of the class:
  - Constructors
  - Destructor
  - Copy and assignment operators
  - Member functions
  - Data members

As always, technical concerns override style, and this order sometimes cannot be
exactly followed for technical reasons, but it should be the predominant
weakly-binding preference when laying out a class in the absence of motivation
to diverge from it.
Private data members have a leading underscore followed by a camel case name like `_fooBarBaz`.
Protected members may or may not have a leading underscore, depending on how
logically internal they are. This convention doesn't apply to types.

### Naming of Class Members

```c++
class Foo {
public:
    // This is just for demonstration purposes. Classes/structs should rarely
    // have a mix of public and private data members.
    int publicMember;

protected:
    // We've never had a convention about protected members. Both are
    // widespread, so either is okay. It depends on how "private" the variable
    // is to the derived classes.
    int x;
    int _y;

private:
    int _privateMember;
};
```

### User-facing Names That Include Units (not strictly a C++ issue)

This section applies to names that users can see, like BSON field names or
server parameters, but not necessarily to C++ identifiers.

In things like `serverStatus`, include the units in the field name if there is
any chance of ambiguity. For example, `writtenMB` or `timeMs`.

- For bytes: use `MB` and show in megabytes unless you know it will be tiny.
  Note you can use a float so `0.1MB` is fine to show.

- Durations:
  - Use milliseconds by default.
    Prefer the suffix `Millis`, but be aware that `Ms` is also used.
  - Use `Secs` and a floating point number for times that are
    expected to be very long.
  - For microseconds, use `Micros` as the suffix (e.g., `timeMicros`).

## Documentation

- API docs should appear directly above the thing being documented and use `/**` or `///` style comments.

- If it fits, a comment can be to the right of a variable with `///< doc`.
  (See [Doxygen syntax](https://www.doxygen.nl/manual/docblocks.html#memberdoc)).
  The `<` is important, as it tells tooling such as clangd to bind backwards to the preceding
  decl rather than the following one.

- We don't run Doxygen or recommend other Doxygen markup, this style of comment
  delimiter distinguishes API docs from other comments.

- Use complete, grammatical sentences for API docs. Reviewers should pay attention
  to the clarity of documentation as it would appear to a reasonably-experienced
  server engineer who may not be a domain expert on the code.

- Avoid overly conversational tone, unnecessary personal references (like "I",
  or "Pat"), slang, or jargon. Comments should strive for professionalism, but
  without rigid formality.

- Comment syntax

```c++
stdx::thread _thread;  ///< Empty until init is called.

/** Single line doc. */
void easyFunction(int x, int y);

/**
 * Multi line doc. Spans multiple lines.
 * The top and bottom lines of this comment block are blank.
 */
void complexFunction(int x, int y) {
    // Interior implementation details use line comments like this.
    return someFunc(x + y);
}
```

- Give the right amount of information. Make some attempt to give the gist of
  complex processes. Avoid being unnecessarily vague to avoid explanation
  that would be helpful to the consumer of the API. Conversely, try to avoid
  going too much into implementation details in doc-comments (or at least
  clearly state when doing so using words like "currently") unless those details
  are part of the API that consumers should rely on.

- Comments should be descriptive rather than imperative, e.g.
  "Frobnicates the widget", not "Frobnicate the widget". The subject of the
  initial sentence is assumed to be the thing being documented and should
  generally be omitted, e.g. don't say "This function frobnicates the widget".

```c++
/** Calculates the sum. (GOOD: descriptive verb) */
/** Calculate the sum. (BAD: imperative verb) */
```

There's no need to be very formal about their formatting or use elaborate
Doxygen/Javadoc etc tags. A smattering of text-like markdown is good. Some IDE
features or other tooling might pick up on it, but it shouldn't interfere with the
primary use case of viewing the comments as text while browsing a header file.

Reader attention is a precious resource, so try to write concise comments, and
obvious things need not get a comment. Comments should be adding information.
Do not restate the name and signature, unless there is a subtle detail that
should be highlighted.

Assume the reader knows the language. Special member functions like the copy
constructor do not need comments saying what they are. `operator==` should only
get a comment if there is something interesting about it like omitting a member,
or being order-sensitive.

Most classes and functions should default to having at least a 1-liner comment,
but sometimes context and good naming can make even that a redundant formality
to be omitted. While this is a subjective decision, remember that later readers
will need more hints than the original implementers.

```c++
    /**
     * If the current command does not override Foo, then it comes from a system-wide default
     * value set by the "foo" server parameter. (GOOD: nonobvious).
     */
    Foo getFoo() const;

    /** Gets the bar (BAD: obvious, no info). */
    const Bar& getBar() const;
```

### TODOs

To cite a ticket as a TODO in the code, use this format, with a short reason for
the link. A Jira bot will create reminders when the cited target ticket is
resolved. The target of the TODO cannot be the current ticket. Suppose
SERVER-12345 was a ticket to fix the frobber, and we're documenting some
workaround code:

```c++
// TODO(SERVER-12345): Remove this code when the frobber works again.
```

In comments, a function may be referred to using just its name `foo`, or by `foo()`,
or `foo(int,int)`, depending on context and whether the other forms are ambiguous.

## C++ Code

Much of the guide has been about cosmetics like layout and formatting, comments, and naming
conventions. This section presents more substantial technical issues.

### Minimal Syntax

If a keyword or operator is a "noise" word with no technical benefit, omit it.
The philosophy here is that it's better to write the code as plainly as
possible. Code should not look like it's doing something special when it isn't.

Some examples of "noise" syntax:

- Redundantly marking members and bases as `public`, `protected` or `private`,
  etc when they already are.
- Marking a function decl to be `extern` (they're already extern).
- Using `virtual` on a function that's already `override` or `final` (see
  "[Overriding Virtuals](#overriding-virtuals)").

### Constructors

Constructors that can be called with single arguments should be `explicit`,
unless implicit conversion is desired, in which case use `explicit(false)` to
explicitly show that intent.
Non-unary constructors should NOT be `explicit` unless it is important to
disable bare braced initialization. If a constructor takes a variable number of arguments
such that it is possibly unary, make it `explicit`.

### `= default`

Prefer `= default;` when needed over defining an empty or trivial function body `{}`.
But where possible, it is usually better to omit the declarations for lifetime methods
entirely and let the compiler declare them implicitly.

Consider that for some classes it may be useful to declare a function normally
in a `.h` file and provide `= default;` as the implementation in a `.cpp` file.

### Noexcept

The `noexcept` feature is easy to overuse. Do not use it solely as "documentation"
since it affects runtime behavior. It's a large topic, covered in the [Exception
Architecture](https://github.com/mongodb/mongo/blob/master/docs/exception_architecture.md#using-noexcept)
document.

### Overriding Virtuals

Use `override` wherever it can be used. Tighten this to `final` when necessary,
and where further overrides would introduce opportunities to break base class
guarantees.

Each declaration should have at most one `virtual`, `override`, or `final`.

Like many style rules, there are rare technical situations to bend this rule. In
this case it can be used to force compilation errors on unintentional hiding.

If a class is known to be a leaf in a hierarchy of polymorphic types, annotating
the class with `final` can be a useful optimization to enable its `virtual`
functions to be devirtualized in some contexts.

### Rules For `.h` Files

- Use `#pragma once` as an include guard, as the first line after the copyright notice.

- No unnamed namespaces in headers at all.
  (See the "Namespaces" section below).

- Use `inline` or `extern` on namespace-scope variables in headers, so that each
  translation unit does not get its own copy. Note that `inline` variables
  provide some init order guarantees which may add a small startup cost, so
  define them as `constexpr` or `constinit` if possible.

- Keep complex code out of headers. If a function is not performance sensitive, and it
  is longer than a few lines, put it in the corresponding .cpp file. This practice
  should help to reduce the number of include statements needed in headers,
  which is good for modularity and for compilation speed. That said, simple
  getters and setters should generally be inline.

### Rules For `.cpp` Files

Entities with "external linkage" are usable from outside the .cpp file where
they are defined. It's the default linkage for functions, variables, and types
defined at namespace scope, making this unintentional exporting a common error
in C++.

Export with intent. Avoid defining anything with external linkage unless it's
declared in the header. We don't want to have surprising link-time name
collisions or other multi-definition problems as the codebase evolves.
When code has no more callers, it can be readily identified as dead code if it has
internal linkage.

Use either unnamed namespaces or `static` to make definitions with "internal
linkage". These are private to the .cpp file in which they appear.
(See "[Linkage](https://en.cppreference.com/w/cpp/language/storage_duration#Linkage)").

### API Conventions

#### Integer Ranks

We don't typically use the `long` or `long long` integer ranks, except in the
BSON API or when interfacing with third_party or system APIs. In particular, we
should never use plain `long` directly unless required by some outside API since
it is 32 bits on some of our supported platforms. We use `int`, `size_t`, and
the explicit width typedefs `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, etc.
Prefer `size_t` for string/array/container/sequence sizes and indexes, since
that's what C++ does.

#### `const`

- Our code uses "west const" (`const X x;`) rather than "east const" (`X const x;`).

- `const` is not required on local variables.

- Making `const` data members of a movable class can lead to problems with
  move and assign operations, and is usually not necessary. On the other hand,
  it can be useful for types that are never moved or copied. In particular, for
  types that are accessed concurrently it is useful to mark members that are
  not modified after construction as `const` because they cannot participate in
  data races.

- Don't use `volatile` qualifications. It's an oft-misunderstood feature and
  only appropriate in very precise technical scenarios.

### Strings

- We do not use `std::string_view`. Use `StringData` from `base/string_data.h` instead.
  For interoperability with functions that accept or return `std::string_view`
  (e.g. `std::string`), use the pair of conversion functions
  `toStdStringViewForInterop` and `toStringDataForInterop`.

- Working with `char*` strings can be notoriously error-prone. Convert such data to
  `StringData` or `std::string` for safety, or use utilities in `util/str.h` for
  this sort of thing.

### Performing String Formatting

There are at least two kinds of generic string formatting available. We have
stream-oriented formatting with `StringBuilder` and its wrapper `str::stream()`
(using a stripped-down `std::ostream`-like API), and newer `libfmt` formatting
(using Python-like syntax). We do not use `std::format`. `sprintf`-style
formatting is very rarely used.

```c++
    #include <fmt/format.h>
    takesString(fmt::format("x={}, y={}\n", xValue, yValue));
```

```c++
    #include "mongo/util/str.h"
    takesString(str::stream() << "x=" << xValue << ", y=" << yValue << "\n");
```

### Output Parameters

Use pointers or mutable references as "in/out" or "output" parameters,
but prefer returning values to using pure output parameters.
Mutable references used to be banned, but this is no longer the case, and
they are now encouraged for many cases, especially if the callee will not
require the reference to be valid after returning. That said, some types,
such as `OperationContext` are conventionally passed by pointer.
It is best to stick to established conventions for such types to avoid
needing a lot of additional `&opCtx` and `*opCtx` noise at call sites
between functions using different conventions.

```c++
void appendData(const std::string& tag, std::vector<MyType>& out) {
    out.push_back(_getData(tag));
}
```

### Namespaces

- Namespace names use `snake_case`. No uppercase letters, and words separated by underscores.

- Contents of `namespace` scopes are not indented.

- Close namespaces with a comment. `clang-format` automatically adds these comments.

  ```c++
  namespace foo {
  int fooVar;
  namespace bar {
  int barVar;
  }  // namespace bar
  }  // namespace foo
  ```

- Do not use "using directives" (i.e. `using namespace foo;`) for arbitrary
  namespaces as a naming shortcut. Some namespaces are designed to be used this
  way in restricted contexts, but still never at namespace-scope in header
  files. These carefully curated namespaces contain only a few definitions.
  Examples of these limited exceptional namespaces would include:

  - The `std::literals`, `fmt::literals`, and similar namespaces that hold
    user-defined literal operators. Using directives are necessary for importing
    user-defined literals.
  - The `std::placeholders` namespace containing `_1`, `_2`, for use with the
    `std::bind` API (which we have banned anyway).

  As an alternative, a namespace _alias_ may help to declutter local scopes.

  ```c++
  namespace bc = timeseries::bucket_catalog;
  namespace bfs = boost::filesystem;
  ```

- No unnamed namespaces in headers at all.
  They can produce subtle correctness risks, particularly in the form of
  [ODR (One Definition Rule)](https://en.cppreference.com/w/cpp/language/definition#One_Definition_Rule)
  violations.

- In .cpp files, use unnamed namespaces to strip definitions of their linkage.
  Headers should generally only be declaring entitiees with external linkage.

- Most server code should be in the `mongo` namespace, and we have several
  sub-namespaces nested within that, often used to help organize code by team, by
  project, or by large feature.

- Defining a new nested namespace as an API point is cheap, but can be a little
  fiddly for users if we have too many of them, so they should be substantial and
  relatively coarse-grained (a handful per team).

- Use a component-unique namespace, eg `future_details` or `duration_detail`, to
  give names to pseudo-"private" details in headers. It's important to include
  the component name here. Using `mongo::detail` or `mongo::internal` doesn't
  mitigate the problem of name collisions between components.

- As a matter of namespace etiquette and modularity, avoid using anything in a
  component's `detail` or `internal` -suffixed namespaces from outside the
  component. If you need to use such a private name, that should ideally involve
  a conversation with the code owners about promoting it out of the detail
  namespace.

- Combine immediately-nested namespace blocks where possible:

```c++
namespace mongo::foo::bar {
int barVar;
}  // namespace mongo::foo::bar
```

### Control flow

- Place exceptional path first.
- Return early.
- Avoid `else` after a returning `if` statement.

```c++
Status ifElseSpaghetti() {
    Status err;
    if (err = doStuff1(); err.isOK()) {
        if (err = doStuff2(); err.isOK()) {
            if (err = doStuff3(); err.isOK()) {
                if (err = doStuff4(); err.isOK()) {
                    // Expected path obscure and indented
                } else {
                }
            } else {
            }
        } else {
        }
    } else {
    }
    return err;
}

Status withEarlyReturns() {
    if (auto err = doStuff1(); !err.isOK())
        return err;
    if (auto err = doStuff2(); !err.isOK())
        return err;
    if (auto err = doStuff3(); !err.isOK())
        return err;
    if (auto err = doStuff4(); !err.isOK())
        return err;
    // Expected path obvious and prominent.
    return Status::OK();
}
```

#### Range-Based `for` Loops

[Range-based for loops](https://en.cppreference.com/w/cpp/language/range-for) can have subtle issues.
The usual practice is to use a forwarding reference (`auto&&`) as the item variable. Applying this
pattern as a default practice prevents subtle copies and conversions of the range elements.

```c++
    for (auto&& item : someRange)
```

For ranges that have pair or tuple elements, particularly maps, it's common to
use structured bindings to give names to the parts of the item:

```c++
    for (auto&& [key, value]: someMap)
```

It's worth a note of caution about the dangers of the range expression in a
range-based for loop, as this is a common and subtle source of bugs.

The range expression is bound to an implicit range variable, and its lifetime
will be extended if it's a temporary, as usual with C++ initializers.

But other temporaries created in the initializer expression will die after the
initializer. They are not extended to the lifetime of the for loop.

```c++
    // ok: temporary is bound to implicit range variable.
    for (auto&& item: makeVector())

    // BUG: the result of obj() is destroyed.
    for (auto&& item: obj().view())
```

The rules here change in C++23, such that all temporaries in the range initializer are extended.
The fix is a theoretically a breaking change for some code. But the risk tradeoff
overwhelmingly favored making this change anyway.

> [!WARNING]
> The compilers we are using have not all implemented this feature yet, even on the v5 toolchain. So
> we still need to be extremely careful with range expressions that rely on
> intermediate temporaries.

It would be helpful to read the [CppReference](https://en.cppreference.com/w/cpp/language/range-for#Temporary_range_initializer) on this topic.
Some good [bug examples](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2644r0.pdf)
are listed in the single-page ISO C++ proposal to fix the problem.

### Assertions

This is a large topic. See the [Exception Architecture](https://github.com/mongodb/mongo/blob/master/docs/exception_architecture.md) architecture guide.

### Logging and Output

We use a custom logging system, documented in the
[Logging](https://github.com/mongodb/mongo/blob/master/docs/logging.md)
architecture guide. Direct output to `stdout` or `stderr` streams is only done
by special server code.

### Numeric Constants

Large, round numeric constants should be written in a user-friendly way.

- If a number is derived from a simple numeric expression, expressing it as an
  expression can help a reader verify and maintain it. For example, prefer
  `50 * 1024 * 1024` to `52'428'800`.

- Use digit separators `'` for large numeric constants. 3-digit groups for
  decimal. Conventionally, use 4-digit or 8-digit groups for hexadecimal or
  binary.

- Use a bit-shifted form for power-of-two exponentiation. eg, `1<<13` to express 2<sup>13</sup>.  
  Make sure the "1" is wide enough for the shift if it's large (e.g. `uint64_t{1} << 52`).
  A `* 1024` sequence is also acceptable, as it's a recognizable idiom for kiB and MiB expressions.

- Do not assume suffixes like `ULL` will produce specifically typed quantities like `uint64`.
  Use a numeric literal and the compiler will give it a wide-enough type.
  Where the exact type matters, use an explicitly typed expression.

```c++
const int tenMillion = 10'000'000;
const int miBiByte = 1 << 20;
const uint64 exBiByte = 1ull << 60;  // Arithmetic expressions may need a particular type.
const uint32 crc32Polynomial = 0xEDB8'8320;
const uint32 asciiMask = 0b0111'1111;
arrayBuilder.append(uint64_t{1234});  // Force argument type.
```

### Casting

- Do not use C-style cast syntax (parentheses around the preceding type) ever.
  See [this CGL rule](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#es49-if-you-must-use-a-cast-use-a-named-cast)
  and [this Google rule](https://google.github.io/styleguide/cppguide.html#Casting) for discussion.

- Use `static_cast` as needed. Use `const_cast` when necessary.

- Be aware that `dynamic_cast`, unlike other casts, is done at runtime. You
  should always check for `dynamic_cast<T*>` returning null pointer.

- `reinterpret_cast` should be used sparingly. It is typically done for
  low-level layout conversions and accessing objects in ways that may break the
  protections of the type system and exhibit undefined behavior if misapplied.

- When down-casting from a base type where the program logic guarantees that
  the runtime type is correct, consider using `checked_cast` from
  `mongo/base/checked_cast.h`. It is equivalent to `static_cast` in release builds,
  but adds an invariant to debug builds that ensures the cast is valid.

### RAII and Smart Pointers

- Embrace RAII (Resource Acquisition Is Initialization). This means that resources
  should generally be managed by objects that automatically release them when
  going out of scope.

- By default, the assumption in our codebase is that raw pointers are
  views/borrows and never owning. Document exceptions to that rule, and try to
  avoid having owning raw pointers as part of your API.

- Make heavy use of smart pointers such as `std::unique_ptr` and `std::shared_ptr`.
  For some types we use `boost::intrusive_ptr` instead.

- Generally, bare calls to `new`/`delete` and `malloc`/`free` outside of the
  implementation of an RAII type should be red flags and draw extra scrutiny in
  review. Prefer factory functions like `std::make_unique` and
  `std::make_shared`.

- Use `ScopeGuard` or `ON_BLOCK_EXIT` to protect other resources that must be
  released (e.g. `fopen`/`fclose` pairs), or perform some other action when
  leaving scope. It is often a good idea to put "undo X" logic right after the
  "do X" logic rather than at the bottom of the function to ensure that the
  logic stays correct if someone adds an early return or throws. Or, write an
  object to do this for you via its constructor and destructor.

### The `WithLock` Convention

It is common practice in our codebase for a larger "business logic" class to
have an obvious primary mutex member. These tend to have some private functions
that require that this mutex be held. These functions often take a
`WithLock` as the first parameter to document the contract and provide some
checking of the callers. The parameter should usually be unnamed. This is a
technical check that forces callers to present a lock-holding resource handle
(e.g. `unique_lock`) to call the function. See
[with_lock.h](../src/mongo/util/concurrency/with_lock.h).

## Files (Physical Design)

### Components

A component is a grouping of classes, entities, and functions that is built as a
single packaged unit. There are 1 or more components in a library. A component
should represent a grouping of functionality and interrelated classes and
functions that work together.

A component normally consists of a `.h`, a `.cpp`, and a `_test.cpp` file.
Source filenames use lowercase words separated by underscores (i.e. snake_case).

In uncommon cases, there are other files in the component for technical or
internal organizational reasons. These might be a `foo_internal.h` auxiliary
header, or a `foo_test_part4.cpp` test fragment, but these extra files are not
meant to serve as its main interface or present its main idea. They're helper
details and they should have the component name as a prefix of their file names.

A component will commonly be dominated by a single dominant class, and for
discoverability, it should therefore use that class name, in snake_case, as its
filename. That said, we have no rule limiting the number of declarations in a
file, and it is useful to define related classes together in a single component.

### Using `#include`

- To make a declaration available, we require inclusion of a header file that
  provides it. There should not be any implicit reliance on transitive includes,
  even if the code compiles. As an exception to this general rule, `foo.cpp` and
  `foo_test.cpp` do not need to duplicate the includes from `foo.h`.

- Do not make forward declarations to avoid an inclusion. It may be tempting to
  do this as an optimization, but we don't do it, as there are correctness and
  modularity risks.

- Do not include headers that are not needed. Do not blindly copy large blocks
  of include statements.

- An "umbrella" interface header may provide several related transitive
  includes, but these umbrella headers should be documented as such, and they
  should be provided by the library maintainer. Use IWYU (include what you use)
  pragma comments to prevent tools and editors from incorrectly auto-suggesting
  the private headers.

  In the public header (e.g. `unittest/unittest.h`):

  ```c++
  #include "mongo/unittest/assert.h"  // IWYU pragma: export
  ```

  In the private headers (e.g. `unittest/assert.h`):

  ```c++
  // IWYU pragma: private, include "mongo/unittest/unittest.h"
  // IWYU pragma: friend "mongo/unittest/.*"
  ```

- A header should also be "self-contained", and include everything it needs. It
  must not rely on other headers having been included above it by its users.

- Use "double quotes" to include headers under `mongo/`, and \<angle brackets\>
  for headers under `third_party/`, or for system libraries.

- Always use the forward relative path from `mongo/src/`. "Forward" means to not
  refer to the parent directory `../`.

- Don't use `third_party/` as part of include paths. Use `<>` and omit it.

  ```c++
  #include <boost/optional.hpp> // Yes
  #include "third_party/boost/optional.hpp"  // No: omit "third_party/" and use <>
  #include "boost/optional.hpp"  // No: use <>

  #include "mongo/db/namespace_details.h" // Yes
  #include "../db/namespace_details.h"  // No: ".." is disallowed
  ```

### Ordering and Grouping of C++ `#include` Directives

We have a standard order for the include directives at the top of a C++ file.
It is automatically applied by our configuration of clang-format.
The purpose of this ordering is to keep the list organized to aid in visual
scanning, and to catch headers that are missing includes.

The include directives are organized into several blocks.
Within each block, the include directives are sorted alphabetically.
Follow each block with a blank line.

- Main header

  For the `.cpp` and `_test.cpp` files of a component, include the component's
  `.h` file if applicable as the first include. This is a safety practice that
  helps us ensure that a `.h` file doesn't rely on any preceding inclusions.

- First-party headers

  All include directives using `""` and starting with `mongo/`.

  E.g. `"mongo/db/db.h"`.

- C++ stdlib headers

  Include directives using `<>`, with no `/` or `.` in path.

  E.g. `<vector>`, `<cmath>`.

- Unnamespaced headers

  Include directives using `<>`, with no `/` in path.
  Typically these are system C headers ending in `.h`

  E.g. `<unistd.h>`.

- Remaining third-party headers

  Include directives using `<>`, with `/` in path.

  E.g. `<boost/optional/optional.hpp>`, `<sys/types.h>`.

To summarize, a typical .cpp file "classy.cpp" might have up to 5 sorted blocks of
include directives:

```c++
/** (Copyright notice would appear at the top, then...) */

#include "mongo/db/classy.h"

#include "mongo/db/db.h"
#include "mongo/db/namespace_details.h"
#include "mongo/util/concurrency/qlock.h"

#include <cstdio>
#include <string>

#include <unistd.h>

#include <boost/thread/thread.hpp>
```

Any headers that are conditionally included under the control of `#if`
directives (if technically possible) will appear after these blocks.

Clang-format will not reorder includes across anything other than a blank line
or other includes. In the rare case where some header must be included before
or after all other headers, you can use a comment line to separate it from
other includes like:

```cpp
#include <last/normal/header.h>

// This header must be after all others:
#include <a/weird/header.h>
```

If you see a comment line in old code that is unintentionally preventing proper
header ordering, you are encouraged to clean that up when adding or removing
includes.

### For `js` Files (JavaScript only)

- Disable formatting for [template literals](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Template_literals)

```js
// clang-format off
newCode = `load("${overridesFile}"); (${jsCode})();`;
// clang-format on
```

### Copyright Notices

- All new C++ files added to the MongoDB code base that will be upstreamed for
  public consumption (such as anything upstreamed to `mongodb/mongo`) should
  use the following copyright notice and SSPL license language, substituting
  the current year for `YYYY` as appropriate:

```c++
/**
 *    Copyright (C) YYYY-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
```

- Enterprise source code is not SSPL, and must bear a shorter copyright notice:

```c++
/**
 *    Copyright (C) YYYY-present MongoDB, Inc.
 */
```

## Basic Formatting Conventions in C++ Code

There are several matters of file formatting expected in source files, and we
enforce these when we can. If you use our recommended
[config](https://github.com/mongodb/mongo/blob/master/.vscode_defaults/linux-virtual-workstation.code-workspace)
for VSCode, much of this will be handled automatically for you.

### Whitespace

- Use spaces, no TAB characters.

- 4 spaces per indentation.

- Limit lines to 100 columns.

- Use Posix text format for source files.
  All lines (including the final line) end with a LF (ASCII "line feed" aka `\n`) character.
  We don't use the Windows CRLF (`\r\n`) line endings in source files.

  In VS Code, `files.eol` should be set to "\n", and `files.insertFinalNewline`
  set to true to help with this. A Git config option on Windows can convert
  line endings automatically (`core.autocrlf`).

### Braces

Our braces style is that the opening brace appears at the end of the line. We
do not open a new line just for the opening brace that is part of a control flow
structure (`if`, `while`, etc).
Braces are optional for sufficiently simple statements.

```c++
    if (condition)
        doStuff();

    if (condition) {
        doStuff();
    }

    while (condition)
        doStuff();

    while (condition) {
        doStuff();
    }

    do {
        doStuff();
    } while (condition);
```

### ESLint (JavaScript only)

All JS files must be linted by ESLint before they are formatted by clang-format.

We use [ESLint](http://eslint.org/) to lint JS code. ESLint is a JS
linting tool that uses the config file located at `.eslintrc.yml`, in the root
of the mongo repository, to control the linting of the JS code.

[Plugins](http://eslint.org/docs/user-guide/integrations) are available for most
editors that will automatically run ESLint on file save. It is recommended to
use one of these plugins.

Use the wrapper script `buildscripts/eslint.py` to check that the JS code is
linted correctly as well as to fix linting errors in the code. This wrapper
selects the appropriate version of eslint to be used.

```sh
python buildscripts/eslint.py lint # lint js code
python buildscripts/eslint.py fix # auto-fix js code
```

### Clang-Format

All code changes must be formatted by
[clang-format](http://clang.llvm.org/docs/ClangFormat.html) before they are
checked in. Use `bazel run format` to reformat C++ and JS code.
Clang-format is a C/C++ & JS code formatting tool that uses the config files
located at `src/mongo/.clang-format` and `jstests/.clang-format` to control the
format of the code. The version and configuration of clang-format is selected by
`bazel run format`.

Plugins are available for most editors that will automatically run clang-format
on file save.

Clang-format is essential, but we should not let it create unreadable code.
There are some ways to keep it from producing a mess:

- It will not join a line that ends in a (potentially empty) `//` comment.
- It also recognizes comma-terminated lists as significant hints.
- As a last resort, it honors `clang-format off` and `clang-format on` in comments.
  This should only be used where it is really important, since it may result in indentation
  drift with the surrounding code as we upgrade clang-format or change settings.

```c++
void clangFormatExamples() {
    // Trailing comma prevents joining braces with data.
    std::array arr{
        123, 234, 456, 567, 678,
    };
    std::vector<std::vector<int>> vvi{
        {
            123,
            345,
        },
        {
            456,
        },
    };

    // Just one leading EOL comment '//' prevents joining all lines.
    b  //
        .append(x, 123)
        .append(y, 234)
        .append(z, 345);
}

// Example tabular data that would be harmed by reformatting.
// clang-format off
#define EXPAND_TABLE(X) \
/*   (id, val          , shortName      , logName   , parent) */ \
    X(kDefault, = 0    , "default"      , "-"       , kNumLogComponents) \
    X(kAccessControl,  , "accessControl", "ACCESS"  , kDefault) \
    X(kAssert,         , "assert"       , "ASSERT"  , kDefault) \
    X(kCommand,        , "command"      , "COMMAND" , kDefault) \
    X(kControl,        , "control"      , "CONTROL" , kDefault) \
    X(kExecutor,       , "executor"     , "EXECUTOR", kDefault) \
    X(kGeo,            , "geo"          , "GEO"     , kDefault)
// clang-format on
```

---

# Additional Learning Resources

- [Learn C++](http://learncpp.com), free C++ tutorial.

- CppCon "Back to Basics" track playlist.
  [link](https://www.youtube.com/playlist?list=PLHTh1InhhwT4TJaHBVWzvBOYhp27UO7mI)

- "A Tour of C++", Stroustrup.
  ISBN: 9780133549003

- "Large-Scale C++: Process and Architecture, Volume 1", Lakos.
  ISBN 9780133927665

- All of Herb Sutter's "Exceptional" series of books.

- All of Alexandrescu books

- All of Scott Meyer's "Effective" books (getting very old but still great)

# References

- [MongoDB C++ Style Guide Proposals](https://docs.google.com/document/d/1nvmEnjw-5DNFIoXPa7WzM1PbOOl1fN19jl1sz9cpzAg)
  Roadmap and suggestion box for this document.

- [Server Code Style](https://github.com/mongodb/mongo/wiki/Server-Code-Style) on mongo github wiki to be replaced by this document.

- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) We used to default
  to this for all things not explicitly covered by our own guide, but that is no longer the case.

- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) Interesting reading.
  Diverges significantly at times from our style.

- [cppreference.com](https://cppreference.com) The best C++ reference site

- [C++ SUPER FAQ](https://isocpp.org/faq)

- [Compiler Explorer](https://goldbolt.org) Great for demonstrating C++ ideas on multiple compilers.

- [VSCode workspace file](https://github.com/mongodb/mongo/blob/master/.vscode_defaults/linux-virtual-workstation.code-workspace)
  A default configuration for server engineers who use VSCode. It's configured
  to handle editor configuration and formatting issues in accordance with this
  guide.
