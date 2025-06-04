# Modules POC

This folder contains a POC implementation of a module metrics tracker and enforcement. This documentation includes
basic information about modules, and commands which will run the scanner across the entire first-party codebase and merge the results. All
commands are assumed to run at the root of the checkout, inside of a correctly activated python
virtual env.

## What is a module

A module:

- Provides a coherent public API
- Has internal details that are not intended to be directly accessed from outside the module
- Is a set of files covering the API (headers), implementations (headers and cpp files), and tests

### Submodules

TODO

## Assigning files to modules

The file `modules_poc/modules.yaml` contains a list of modules, each containing
a list of files. Each file must be contained in only one module. Note that
module assignment is not required to map neatly to team ownership.

In cases where multiple globs match a file, the current rule is that the
longest glob wins. This is used as a simpler-to-implement version of
most-specific glob wins, which we may switch to in the future.

When submitting a review, you are strongly encouraged to include
a generated diff of the changes to the modules list. This can be done by running:

```bash
modules_poc/mod_scanner.py --dump-modules-list > modules.old
# make your changes
diff -u0 modules.old <(modules_poc/mod_scanner.py --dump-modules-list)
```

Github will nicely format the diff if you put it in a block like this:

````markdown
```diff
@@ -1234 +1234 @@
-path/to/file -- old_module
+path/to/file -- new_module
```
````

<!--
  if you are reading the markdown source, ignore the lines with 4 backticks (````),
  and only use a single wrapping with ```diff
-->

### Showing assigned and unassigned files

Run `modules_poc/mod_scanner.py --dump-modules` to produce a `modules_dump.yaml`
file in current directory. This file is a multi-level map from
module name to team name to directory path to list of file names.
For unassigned files it uses `__NONE__` as the module name, and for unowned
files it uses `__NO_OWNER__` as the team, both of which conveniently sort first.
For owned files it uses the part of the team-name after `@10gen/` with `-`
replaced with `_` to be friendlier to querying. In cases where multiple teams
own a file, the file is duplicated to each team's list.

This file can be viewed directly in VSCode. The yaml plugin's breadcrumbs and
folding are very helpful. [`yq`](https://github.com/kislyuk/yq)
([`jq`](https://jqlang.org) for yaml) is also a powerful tool. Here are a few
examples using it, some of which produce enough output to be worth opening in vscode:

```bash
# list of teams
yq '[.[] | keys] | add | sort | unique[]' -r modules_dump.yaml
# unassigned files owned by server-programmability
yq '.__NONE__.server_programmability' modules_dump.yaml
# files owned by server-programmability across all modules (or lack thereof)
yq '.[] |= (.server_programmability | values)' modules_dump.yaml
# assigned files owned by server-programmability outside of the core module
yq '.[] |= (.server_programmability | values) | del(.core) | del(.__NONE__)' modules_dump.yaml
# assigned files owned by server-programmability in modules that don't start with core
yq '.[] |= (.server_programmability | values) |  with_entries(select(.key | startswith("core") | not)) | del(.__NONE__)' modules_dump.yaml
# unowned files as a flat list
yq '.[].__NO_OWNER__ | values | to_entries | map("\(.key)/\(.value[])") | .[] ' modules_dump.yaml -r | sort
# unowned files grouped by directory
yq '[.[].__NO_OWNER__ | to_entries? | .[]] | group_by(.key) | map({key: .[0].key, value: ([.[].value] | add | sort)}) | from_entries' modules_dump.yaml
# assigned files owned by non server-programmability teams inside of the core module (grouped by teams, then directory)
yq '.core | with_entries(select(.key != "server_programmability"))' modules_dump.yaml
```

## Specifying public and private module APIs

To make an API or class available for use by other modules, add a
tag to its header declaration.

```
class MONGO_MOD_PUB Foo {

};

MONGO_MOD_PUB int foo();
```

Availability specification can also be done at the namespace level.

```
namespace MONGO_MOD_PRIVATE my_details {

} // namespace MONGO_MOD_PRIVATE my details
```

Elements inside a class or namespace default to the visibility of the
enclosing scope. Note that the canonical version of "inside" can be
subtle, with, e.g., member functions being "inside" the class definition,
not the location the member function is defined. All forward
declarations of the same function or class should have the same visibility
tags, and forward declarations across module boundaries should be avoided.

If visibility is not specified at any containing scope,
it defaults to `MONGO_MOD_PRIVATE` (except in cases where the
header doesn't include `mongo/util/modules.h`, where the default is `UNKNOWN`
to facilitate incrementally tagging APIs).

Documentation for individual `MONGO_MOD_*` tags is present in
[`mongo/util/modules.h`](../src/mongo/util/modules.h).

### Running the scanner

This will build the `merged_decls.json` file in the current directory:

```bash
buildscripts/poetry_sync.sh # make sure the python env has the right packages installed
python modules_poc/merge_decls.py
```

`merge_decls.py` takes an optional flag `--intra-module` if you want to include intra module accesses
and declarations that are only used from within their module. Typically, you don't so it defaults to
omitting them.

Running `merge_decls.py` also validates that private APIs aren't being used
outside their own module. If any are, the script will fail, though
`merged_decls.json` will still be generated, and the
invalid uses will be printed to stdout.

If you only wish to include the files linked in to a given executable, replace the `bazel build` command with the following commands:

```bash
TARGET="//src/mongo/db:mongod"
bazel cquery --config=mod-scanner "filter(//src/mongo, kind(cc_*, deps($TARGET)))"  | awk '{print $1}' > targets.file
bazel build --config=mod-scanner --target_pattern_file=targets.file
```

### Note for implementers

You can also scan a single file which is useful when iterating on this. You can
either pass it the same flags used to compile, or pass it just a cpp file and it
will figure out the flags from your `compile_commands.json`. It will create a file
called `decls.yaml` to the current directory when run this way.

```bash
modules_poc/mod_scanner.py src/mongo/bson/bsonobj.cpp
```

## Browsing

Once you have produced a `merged_decls.json` file, you can browse it by running
`modules_poc/browse.py`. It will show the available keybindings on the right, which can be toggled
by pressing <kbd>?</kbd>. If you are running from a VSCode or neovim terminal, you can press
<kbd>g</kbd> to go to any location in your editor. You can also press <kbd>p</kbd> to toggle an
embedded preview of the location the current line is currently on (you probably want to hide the
help when doing this). You can press <kbd>Tab ↹</kbd> to switch between the tree and preview.

The browser is primarily intended to assist in labeling public APIs, so the files are sorted with
the most number of unlabeled declarations ("unknowns") first. Only declarations that are used outside of their
module are counted and shown. You can search for a file by pressing <kbd>f</kbd> or press
<kbd>m</kbd> to filter the files by module.

As an advanced feature, you can pass a custom file to `browse.py` and it will
use it rather than the default `merged_decls.json`. It does need still to have
the same shape as the original. This works best with [jq] filtering to do
advanced filtering. For example, here is a command that will only show
declarations where some TUs will only see a forward declaration from another
module, and will assume that that module is the owner (we need to fix this):

```bash
./modules_poc/browse.py <(jq '[.[] | select(.other_mods)]' merged_decls.json)
```

In general, your `jq` query should be of the form `[.[] | select( SOME QUERY )]`
to avoid breaking the format expectations. For more advanced analysis, using
`jq` directly is a good idea.

## Uploading

Run the following command to upload

```bash
python modules_poc/upload.py $MONGO_URI # fill this in
```

If the upload fails with an error connecting and you need to update the IP
whitelist for your virtual workstation, `curl -4 wtfismyip.com/text` is a good
way to see your public IP address

## Future Work

- [ ] Once we no longer have errant forward declarations in the wrong module, we can
      make the processing a lot faster by having the scanner only write out things
      that are used across modules (or in the `__NONE__` module if we still want to
      track that).
- [ ] We should explore if using the indexing API (eg `clang_indexSourceFile`) will
      yield better results. In particular, there is a flag to opt-in to visiting all
      implicit instantiations which I think is currently a blind spot. Unfortunately
      it isn't exposed in the python API yet, so we would need to add it there
      first.
- [ ] Other interesting options would be a clang-tidy plugin or a clang plugin. We
      already have a lot of infrastructure to support clang-tidy plugins, but they
      will ignore any lines with `// NOLINT` comments. A clang plugin is
      particularly interesting if it will be able to run inside of clangd so we can
      show warnings when accessing an unfortunately-public API across modules (we
      may want to mark it as deprecated in that case) and errors when accessing
      private.
- [x] Parallelize the merge script. Right now it is single threaded. We can use
      `multiprocessing` or similar to parallelize it. It should use a queue of input
      files and have workers merge them into a local `all_decls` map and then the
      main thread should merge the results from each worker. If we just pass the
      path to the workers, this will also parallelize reading the files and parsing
      the json.
- [x] We should try to report `loc` as the header declaring the entity, but right
      now it will report the cpp file where it is defined. This is currently
      important since we use the definition to decide the canonical location and
      module when merging. This may cause issues for free functions if the namespace
      is marked public in the header. The latter issue can be worked around when
      merging by using the visibility from other files if the current visibility is
      unknown. But we should pick the right location for `loc` regardless.
- [ ] Try to collapse template instantiations that clang's `specialized_template`
      helper fails to. I don't know why it fails to, but it seems to on many of
      our templates. Maybe we should just merge all declarations with the same `loc`?
      If we do that, we should try to prefer decls where `kind` is a template.
- [ ] Split "unfortunately public" into 3 categories:

  1. `NEEDS_REPLACEMENT`: The current API isn't ideal for a public API, but consumers need its
     functionality. It is on the module owner to provide a better API.
  2. `USE_INSTEAD("replacement")`: A replacement for this API has been provided that external
     consumers should switch to. It is on the module consumer to update their code.
  3. `CURRENTLY_USED`: This is a marker we can put on code as we improve the scanner if it finds
     new usages of private APIs that were hidden in older versions. The module owner should examine
     these and decide if they should be public or marked for replacement.

- Browser enhancements:
  - [x] Add some search functionality to the browser, at least for files.
  - [ ] Add a way to show a flat list of all decls in a file, rather than
        the nested view.
  - [x] Include `spelling` in the output from the scanner, and use that tol
        highlight that part of the decl in the list
  - [ ] Have scanner include the source ranges (extents) of each usage so that
        we can highlight correctly in the viewer. Should also include the name
        of the entity performing the usage. Probably best to use something like
        `pretty_location()` but with `spelling` rather than `display_name`
  - [ ] Make it easier to filter and sort the decls for exploration. One way would be
        to use [jq](https://jqlang.github.io/jq/) expressions. Need to be careful that
        the data is still in the original shape. Could take two expressions, one that
        goes into a [`select()`](https://jqlang.github.io/jq/manual/#select) and another
        that goes into a [`sort_by()`](https://jqlang.github.io/jq/manual/#sort-sort_by).
        For now you can do `browse.py <(jq '[.[] | select( ... )]' merged_decls.json)`, but we
        should come up with something better.
