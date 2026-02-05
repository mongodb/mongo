# Modules API

## What is a module

A module:

- Provides a coherent public API
- Has internal details that are not intended to be directly accessed from outside the module
- Is a set of files covering the API (headers), implementations (headers and cpp files), and tests

### Submodules

TODO

## Why are we doing this?

Having a clear delineation between public and private APIs for each module will improve the
maintainability and velocity of our codebase. Teams will have more freedom to evolve their
internal implementation details without affecting consumers. Consumers will benefit from
knowing what APIs are intended for their consumption.

## Assigning files to modules

The file `modules_poc/modules.yaml` contains a list of modules, each containing
a list of files. Each file must be contained in only one module. Note that
module assignment is not required to map neatly to team ownership.

In cases where multiple globs match a file, the current rule is that the
longest glob wins. This is used as a simpler-to-implement version of
most-specific glob wins, which we may switch to in the future.

## How do I mark API visibility?

This section will just describe the basic process. Later sections will cover the tooling
available to help, along with caveats to be aware of.

First read the documentation in [src/mongo/util/modules.h](https://github.com/mongodb/mongo/blob/master/src/mongo/util/modules.h)
for the canonical list and description of visibility levels. As a brief overview of the main
levels from least to most restrictive:

- `OPEN`: This is available for usage _and inheritance_ from anywhere in the codebase
- `PUBLIC`: This is available for usage from anywhere in the codebase. For types, subclasses may
  only be defined in the same module.
- `NEEDS_REPLACEMENT` and `USE_REPLACEMENT(...)`: These are collectively considered
  "unfortunately public" and are available for use, but should be avoided
- `PARENT_PRIVATE`: This is similar to `PRIVATE`, but allows usage from any file in the parent
  module, including other submodules
- `PRIVATE`: This may only be used from the current module or one of its submodules
- `FILE_PRIVATE`: This may only be used from the current "file family" (roughly, header \+ cpp
  \+ tests). It may not be used by other files, even from the same module.

You can think of public vs private similarly to how you would the sections of a `class`: they
indicate whether something is intended to be part of the API or an implementation detail. The
difference is that they apply at a wider granularity of code than a single class, with
implementation details available to either the full module (and its submodules) for `PRIVATE`
or the file family for `FILE_PRIVATE`.

The macros in that header file are attached to declarations and set the visibility level for
that declaration and all of its "semantic children"[^1]. The macros are C++ attributes which
means that they need to go in specific places that differ based on what is being marked (for
templates, the location does not change and is always somewhere after the `template <...>` part):

- `MONGO_MOD_PUBLIC;` by itself as the first line after includes in a header sets the default
  for that header (only `PUBLIC`, `PARENT_PRIVATE`, and `FILE_PRIVATE` are allowed here)
- `namespace MONGO_MOD mongo {` (this does not work with nested namespaces in a single
  declaration like `namespace mongo::repl`)
- `class MONGO_MOD Foo {` (Ditto for `enum`, `struct`, and `union`)
- `MONGO_MOD void func(...);`
- `MONGO_MOD int var;`
- `concept isFooable MONGO_MOD {`

For the cases where it goes at the beginning of the line, if clang-format chooses an unfortunate
place to break the line, it usually helps to undo the formatting then put the macro on its own
line above the declaration.

APIs are marked one header at a time, by including `"mongo/util/modules.h"` in the header.
This causes the header to be treated as "modularized" which has the following effects:

- All declarations in that header (not transitive includes) default to `PRIVATE`, meaning that
  the public API is what must be marked.
- Members in `private:` sections in classes default to `PRIVATE`, regardless of the visibility
  of the class. The only way the language would allow them to be used from outside of the module
  is if you have cross-module friendships, which should generally be avoided. If needed
  temporarily, favor `NEEDS_REPLACEMENT` over `PUBLIC` for these declarations.
- Declarations ending in `_forTest` default to `FILE_PRIVATE` to support the common case where
  they are only intended for testing that class. If they are actually intended to support testing
  of consumers, not just the type they are defined on, they can be explicitly given `PUBLIC` or
  `PRIVATE` visibility.
- Internal and detail namespaces default to `PRIVATE` and cannot be made less restricted, but
  can still be marked as `FILE_PRIVATE`. Individual declarations within the namespace can be
  exposed as necessary, but they cannot be exposed in bulk without changing the name of the
  namespace to something that doesn't imply private.

For internal headers of a module which do not contribute to its public API, simply including
`modules.h` is sufficient. There is a [tool](#the-private-header-marker) to automate this
process. You may additionally want to consider whether any APIs should be marked `FILE_PRIVATE`,
but that is optional.

For IDL files, you mark visibility of whole types (`struct`, `enum`, and `command`) with the
`mod_visibility` option. The value should be the same as one of the `MONGO_MOD` macros, but
lowercase and without the prefix, for example `mod_visibility: public`. You can set the default
visibility for all types in that IDL file by putting that in the `global:` section. You cannot
control visibility of individual functions within the type. Please let us know if you have a
compelling use case for this.

## What tooling exists to help me?

Note that all tooling should be run from within a properly set-up python virtual environment.
This includes running `buildscripts/poetry_sync.sh` to ensure you have the correct dependencies.

### The scanner and merger

The merger generates a cross reference of all first-party usages of first-party code and stores
it in `merged_decls.json`, which is used by the rest of our tooling. It is also where we validate
that there are no disallowed accesses. It will be invoked for you by the browser when you ask it
to rescan, or you can also manually run it as `modules_poc/merge_decls.py`. If you are interested
in analyzing that file, [`jq`](https://jqlang.org/) is a powerful tool, or you can just write
some python.

As a rather extreme example of what you can do with `jq`, here is how the progress reports are
generated:

```shell
# For each mod (and TOTAL):
#   For each file:
#     consider it marked if it has no UNKNOWNs
#   Compute a done percentage
#   Format to a nice string
jq 'map(., .mod = "TOTAL") | group_by(.mod)[] | group_by(.loc | split(":")[0]) | {mod: .[0].[0].mod, total: length,  marked: map(select(any(.visibility == "UNKNOWN") | not)) | length} | .done = (1000 * .marked / .total | round) / 10 | "\(.mod): \(" " * (.mod | 40-length)) \(.done)%  (\(.marked) / \(.total))"' -r merged_decls.json
```

Internally, the merger will internally invoke `bazel build --config=mod-scanner //src/mongo/...`
to run the scanner over the whole codebase (or the parts that have changed since the last scan),
taking advantage of bazel remote execution to achieve very high levels of parallelism.

### The browser

The main piece of tooling to run is the browser, which is launched by running
`modules_poc/browse.py`. If you haven't scanned the codebase recently, it will offer to run it
for you which will take a few minutes. After modifying the source code, you can rescan at any
time by pressing `r`. It will only rescan files that have been modified or that transitively
include modified headers.

The browser is primarily intended to assist in labeling public APIs, so the files are sorted
with the most number of unlabeled declarations ("unknowns") first. You can search for a file
by pressing `f` or press `m` to filter the files by module.

The list of available key bindings is shown on the right. You can toggle that by pressing `?`.
Other keybinding of note are that you can press `g` to go to the currently highlighted
declaration or location in your editor (only when running in the vscode or nvim terminal),
and `p` to toggle an inline preview of the location within the browser. You can press `Tab ↹`
to toggle between the tree and the code preview. The mouse is fully supported for scrolling
and expanding rows in the tree, and there are aliases for some basic vim keybinds (`hjkl/`).

### The private header marker

Once you have scanned the codebase and produced a `merged_decls.json`,
`modules_poc/private_headers.py` can be used to find all header and IDL files where there are
no currently detected external usages and automatically mark them as fully private to the
module. This does not necessarily mean that all automatically marked headers are intended to
be private. A human should review to ensure that the marked headers match intent. You can pass
flags to filter on any/all of module, owning team, or path glob. For headers matching the filter,
the script will also warn of usages of `_forTest` external to the file family that may need to
be marked `PRIVATE` to make them available to the whole module since they default to only being
available to the file family for marked headers.

Make sure to run `buildscripts/clang_format.py format-my` or `bazel run format` after using it
to modify any C++ files.

Example usage:

```shell
./modules_poc/private_headers.py --team=server_programmability --module=core --glob="src/mongo/executor/*"
```

`--dry-run` can be added to view all of the changes without applying them.

### The PR comment generator

You can run `modules_poc/mod_diff.py` to output a brief summary of all of the API (including
visibility levels and usages counts) for each file modified in your branch. When putting up a PR
to mark API visibility, you should add a comment with its output to the PR as an aide to
reviewers. The output is intended to be close enough to C++ that you should put it in a
` ```cpp ` block when making your PR comment to make it more readable. You can also
pipe it through `bat -lcpp` to make it colorful locally. Note that it will use the last
scan output, so if you've modified any headers, you should run a rescan prior to running this
tool.

## Workflow

The general workflow for each PR will generally be the same:

1. Ensure that you are in a python virtualenv, creating one if needed, and run
   `buildscripts/poetry_sync.sh` to update python deps.
2. Run [the merger](#the-scanner-and-merger) to scan the code base: `modules_poc/merge_decls.py`
3. Mark some headers
4. Rerun the merger to ensure that there are no violations, and update `merged_decls.json`
5. Run [the pr comment generator](#the-pr-comment-generator) to show the APIs that you have marked
   - Look through this to ensure that everything is as you expect.
6. Put up a PR and include the generated comment in a ` ```cpp ` block
   - I suggest keeping PRs small (say, no more than 10 files at a time) so that they are
     manageable by reviewers. As an exception it seems reasonable to auto-mark many headers as
     private in a single PR, as long as those PRs are separate from those containing any manual
     marking.

When first starting to mark a module, I suggest running the [`modules_poc/private_headers.py`](#the-private-header-marker)
script with `--dry-run` (or `-n`) and `--module=YOUR_MODULE`. For larger modules (in particular,
the `query` mega module) you may want to pass a `--glob` so that you can focus on a smaller
subset of the code initially. That will give you an overview of the files that are used from
outside your module (which contain defacto public APIs today) and those that do not (which can
automatically be marked as private implementation details).

If all of the defacto private headers seem like they should be private, you can remove the
dry-run flag to have it automatically mark them as private. Be sure to validate that their
contents are actually intended to be private. Remember that the point of having a human doing
the marking is to ensure that we correctly capture intent. You can optionally mark implementation
details within each header as `FILE_PRIVATE`, if you would like to prevent them from being used
elsewhere even within the module.

You can then open [the browser](#the-browser) (`modules_poc/browse.py`) to look at the remaining
headers. It will show you what is used and from where. It will be particularly useful for things
that seem like they should be private, but are being used externally.

### What should I do when an internal API is currently being used?

1. If it is only used from a small number of external files, first check if those files should
   actually belong to your module. We tried to correctly map all files in phase 1, but some files
   may have been assigned to the wrong module. If that happens, try adjusting the globs in
   `modules_poc/modules.yaml` to move them.
2. If there is already a public API that callers should use instead, mark it as
   `USE_REPLACEMENT(better_api)`. The argument accepts any C++ tokens, but the intent is where
   possible to use the name of the replacement. This will generate a ticket for all teams using
   that code.
   1. If there are very few users, consider just cleaning them up.
3. Reconsider making this API public if other modules need its functionality, and this is
   the only way to get it.
4. Otherwise, if there is no public API that fulfills the needs of the callers, but you
   don't want the current API to remain public long-term, use `NEEDS_REPLACEMENT`. This will
   generate a ticket for the team that owns that code.
   1. If the API was "obviously" intended to be private (eg it is in a `details` namespace)
      and callers would be reasonably able to implement the functionality themselves, possibly
      by writing their own version, it seems acceptable to use
      `USE_REPLACEMENT(do not use internal details)`

## Caveats and Limitations

**OVERARCHING GUIDELINE**: Always try to mark declarations correctly according to intent,
even if it will not be enforced by the current tooling. This is both to provide the correct
information to human readers, as well as to avoid issues if we improve the tooling in the
future to eliminate these limitations

The rest of this section is fairly technical and probably not necessary for most readers unless
they notice something "weird" going on and want to dive into why. Most of these limitations are
more likely to affect the core modules since most of the rest of our code does not expose APIs
via macros and templates or have APIs only consumed by templates, and those are where most of
these issues come up.

- We do not track usages of namespaces at all, only the declarations within namespaces. When
  a namespace is marked with a visibility, it does not affect the visibility of the namespace
  itself (since it doesn't have one), it sets the default visibility for all declarations within
  **that namespace block**. Each time a namespace is reopened it is a separate block and the
  visibility markers on other blocks of the same namespace do not apply.
- The scanner only knows about declarations that it sees being used. For implementation reasons,
  it only discovers declarations by seeing what every usage is using. This can either cause or be
  caused by other limitations.
- Usages in templates may not be seen. This is especially the case for "dependent types and
  values" which are things that are not known by the compiler before the template is instantiated.
  - This is a problem for functions where any arguments are dependent if it can't figure out
    which overload will be selected. It is even worse for free-functions called unqualified
    (`f(blah)` rather than `ns::f(blah)` or `x.f(blah)`) since due to ADL, overload resolution
    is _always_ delayed for them.
- Everything that results from a macro expansion is treated as-if it was written at the point
  of expansion. This applies to both declarations and usages. If you have an API that should
  only be used via the defined macros, mark it as `MOD_PUBLIC_FOR_TECHNICAL_REASONS` to signal
  to readers that they should avoid direct usage, even if the tooling won't prevent it. We may
  improve this in the future.
- Template variables are completely ignored due to some unfortunate clang bugs. Still, try
  to mark them correctly since we may change this in the future.
- Method calls are assigned to the static type at the call site. This has two important effects:
  - A subclass's overridden method may seem unused if it is only used via calls through a base
    class pointer/reference
  - Calls through a base class pointer/reference count as calls of that class's method, not of
    the interface's
- Defaulted members (methods, ctors, dtors) are treated as usages of the class itself,
  regardless of whether they implicitly or explicitly defaulted. This is because clang does not
  provide an API to distinguish between those cases.
- Template normalization woes: we try really hard to report declarations as the template
  `foo<T>` rather than separate instantiations like `foo<int>`, `foo<string>`, etc, **unless**
  they are explicitly specialized, meaning that the instantiation has its own definition different
  from the main template. Unfortunately, clang does a bad job at this and we have a number of
  kludgy workarounds. The most important effects:
  - Explicit specializations of function and variable templates are ignored and always converted
    to the primary template.
  - We do treat explicit specializations of types as separate (using the heuristic of having a
    separate location than the main template), because they can have a different shape and API than
    the main template. In general they should probably have the same visibility though, unless the
    instantiation is using a private type which should be unavailable to consumers anyway.
  - Clang assigns many locations to the site of explicit template instantiations and extern
    template declarations, even when there is a better location that it can see. Luckily these
    are fairly rare.
- Sometimes clang reports the resolved destination of `using` declarations and type alias, but
  usually it reports the `using` declaration itself. A few notable cases (these are trends and
  may not be absolute\!)
  - `using Base::foo;` to expose a member of a base class is resolved as a usage of `Base::foo`
    rather than `Derived::foo`. This is especially notable when the `Base` class is intended to be
    a private implementation detail. You will need to mark all exposed methods as public.
  - `using Base::Base;` to pull in the base constructors is the opposite and is recorded as a
    usage of `Derived::Base(args)`, which is odd because such a declaration doesn't actually exist.
- Internal/details namespaces (currently defined as matching the regex `(detail|internal)s?$`)
  implicitly have implicit default visibility of private if `modules.h` is included. It is not
  possible to give the namespace a public visibility, but you can restrict it further with
  `FILE_PRIVATE`. If you want declarations inside it to be usable from outside your module you
  must mark children of the namespace explicitly, or rename it to not use a name that implies
  that it is for internal usage only. A somewhat common case will be marking internal declarations
  that are only intended to be used via macros with `PUBLIC_FOR_TECHNICAL_REASONS`.
- Be very careful with forward declarations. Try to avoid them wherever possible (unless there
  is a significant benefit). Especially avoid forward declaring anything from another module\!
  Where forward declarations must be used, make sure that they have the same visibility as the
  real definition. As an exception, if every TU that sees the forward declaration will also see
  the definition it is OK to omit marking the forward definition. This may happen when they are
  both in the same header, or the forward declaration is in a private implementation detail header
  which is included by the defining header. Be aware of the implicit visibility marking which also
  applies to forward declaration, if they are the only declaration seen in the TU.
  - Never forward declare functions to avoid including a header. They are much more problematic
    than types, both in general in C++ and specifically for this tooling.
- We try to use the definition location for types defined in headers, but the "canonical"
  location (clang's term for the first declaration seen in the current TU) for everything else.
  If the type is defined in a .cpp, we use the canonical location.
- We only consider declarations in headers, never in .cpp files.
- Be mindful of `_forTest` functions. They default to `FILE_PRIVATE` since they are typically
  intended only for use when testing the type they are defined on, not when testing consumers.
  In the cases where they _are_ intended as part of the API for testing consumers, you can
  explicitly mark them `PUBLIC` or `PRIVATE` depending on whether they should be usable from
  outside your module or not.
- Things used implicitly (eg implicit conversion operators) are still counted as usages even
  if they are not specifically named at the call site
- When merging information from multiple TUs, definitions always replace the metadata gathered
  from TUs that only saw a declaration.
  - Note that we aren't guaranteed to see every definition, in particular for functions that
    are not called from the TU that they are defined in. So this cannot be used to find places
    where we deleted the definition but forgot to delete the declaration (we wouldn't see them
    anyway, since we only track things that are used, and undefined things can't really be used,
    except trivially, without breaking the build).
- `private` members of classes are implicitly `PRIVATE`, and must be explicitly marked otherwise
  if desired. They should probably never be made `PUBLIC` since that implies cross-module
  friendship. In the few places where we have that today, they have been made one of the flavors
  of unfortunately public: `NEEDS_REPLACEMENT` or `USE_INSTEAD`.
  - `public` members of `private` types do not inherit the implicit `PRIVATE` and follow the
    normal rule of looking for their nearest semantic parent with an explicit marker. That means
    that they may be `PUBLIC`. However, the language rules still apply and as long as an
    instance of the type is never handed to consumers they will have no way of accessing those
    members.
  - `protected` members do not default to `PRIVATE`, but because we only allow subclassing from
    `OPEN` classes, the language visibility rules will disallow access from outside the module
    unless you choose to allow it by use `OPEN` classes or `friend`s. Note that making any
    subclass `OPEN` exposes all `protected` members of parents unless they are marked `PRIVATE`.
- `friend` declarations are mostly ignored, except when they are a definition. So the
  definitions using the "hidden friend" pattern are tracked, but we ignore it if the definition
  is in a cpp file.

[^1]:
    Clang distinguishes between "semantic" and "lexical" parents. The primary differences
    are that members of classes (including member types) are semantic children of the class even
    when defined out of line, and conversely `friend` declarations are not, and instead are
    considered semantic children of the nearest namespace.
