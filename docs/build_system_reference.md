# MongoDB Build System Reference

## MongoDB Build System Requirements

### Recommended minimum requirements

### Python modules

### External libraries

### Enterprise module requirements

### Testing requirements

## MongoDB customizations

### SCons modules

### Development tools

#### Compilation database generator

### Build tools

#### IDL Compiler

### Auxiliary tools

#### Ninja generator

#### Icecream tool

#### ccache tool

### LIBDEPS

Libdeps is a subsystem within the build, which is centered around the LIBrary DEPendency graph. It tracks and maintains the dependency graph as well as lints, analyzes and provides useful metrics about the graph.

#### Design

The libdeps subsystem is divided into several stages, described in order of use as follows.

##### SConscript `LIBDEPS` definitions and built time linting

During the build, the SConscripts are read and all the library relationships are setup via the `LIBDEPS` variables. Some issues can be identified early during processing of the SConscripts via the build-time linter. Most of these will be style and usage issues which can be realized without needing the full graph. This component lives within the build and is executed through the SCons emitters added via the libdeps subsystem.

##### Libdeps graph generation for post-build analysis

For more advanced analysis and linting, a full graph is necessary. The build target `generate-libdeps-graph` builds all libdeps and things which use libdeps, and generates the graph to a file in graphml format.

##### The libdeps analyzer python module

The libdeps analyzer module is a python library which provides and Application Programming Interface (API) to analyze and lint the graph. The library internally leverages the networkx python module for the generic graph interfaces.

##### The CLI and Visualizer tools

The libdeps analyzer module is used in the libdeps Graph Analysis Command Line Interface (gacli) tool and the libdeps Graph Visualizer web service. Both tools read in the graph file generated from the build and provide the Human Machine Interface (HMI) for analysis and linting.

#### The `LIBDEPS` variables

The variables include several types of lists to be added to libraries per a SCons builder instance:

| Variable              | Use                                    |
| --------------------- | -------------------------------------- |
| `LIBDEPS`             | transitive dependencies                |
| `LIBDEPS_PRIVATE`     | local dependencies                     |
| `LIBDEPS_INTERFACE`   | transitive dependencies excluding self |
| `LIBDEPS_DEPENDENTS`  | reverse dependencies                   |
| `PROGDEPS_DEPENDENTS` | reverse dependencies for Programs      |

_`LIBDEPS`_ is the 'public' type, such that libraries that are added to this list become a dependency of the current library, and also become dependencies of libraries which may depend on the current library. This propagation also includes not just the libraries in the `LIBDEPS` list, but all `LIBDEPS` of those `LIBDEPS` recursively, meaning that all dependencies of the `LIBDEPS` libraries, also become dependencies of the current library and libraries which depend on it.

_`LIBDEPS_PRIVATE`_ should be a list of libraries which creates dependencies only between the current library and the libraries in the list. However, in static linking builds, this will behave the same as `LIBDEPS` due to the nature of static linking.

_`LIBDEPS_INTERFACE`_ is very similar to `LIBDEPS`, however it does not create propagating dependencies for the libraries themselves in the `LIBDEPS_INTERFACE` list. Only the dependencies of those `LIBDEPS_INTERFACE` libraries are propagated forward.

_`LIBDEPS_DEPENDENTS`_ are added to libraries which will force themselves as dependencies of the libraries in the supplied list. This is conceptually a reverse dependency, where the library which is a dependency is the one declaring itself as the dependency of some other library. By default this creates a `LIBDEPS_PRIVATE` like relationship, but a tuple can be used to force it to a `LIBDEPS` like or other relationship.

_`PROGDEPS_DEPENDENTS`_ are the same as `LIBDEPS_DEPENDENTS`, but intended for use only with Program builders.

#### `LIBDEPS_TAGS`

The `LIBDEPS_TAGS` variable is used to mark certain libdeps for various reasons. Some `LIBDEPS_TAGS` are used to mark certain libraries for `LIBDEPS_TAG_EXPANSIONS` variable which is used to create a function which can expand to a string on the command line. Below is a table of available `LIBDEPS` tags:

| Tag                                                     | Description                                                                                        |
| ------------------------------------------------------- | -------------------------------------------------------------------------------------------------- | -------------------------------- | ------------------------------------------------------------------------------------- |
| `illegal_cyclic_or_unresolved_dependencies_allowlisted` | SCons subst expansion tag to handle dependency cycles                                              |
| `init-no-global-side-effects`                           | SCons subst expansion tag for causing linkers to avoid pulling in all symbols                      |
| `lint-public-dep-allowed`                               | Linting exemption tag exempting the `lint-no-public-deps` tag                                      |
| `lint-no-public-deps`                                   | Linting inclusion tag ensuring a libdep has no `LIBDEPS` declared                                  |
| `lint-allow-non-alphabetic`                             | Linting exemption tag allowing `LIBDEPS` variable lists to be non-alphabetic                       |
| `lint-leaf-node-allowed-dep`                            | Linting exemption tag exempting the `lint-leaf-node-no-deps` tag                                   |
| `lint-leaf-node-no-deps`                                | Linting inclusion tag ensuring a libdep has no libdeps and is a leaf node                          |
| `lint-allow-nonlist-libdeps`                            | Linting exemption tag allowing a `LIBDEPS` variable to not be a list                               | `lint-allow-bidirectional-edges` | Linting exemption tag allowing reverse dependencies to also be a forward dependencies |
| `lint-allow-nonprivate-on-deps-dependents`              | Linting exemption tag allowing reverse dependencies to be transitive                               |
| `lint-allow-dup-libdeps`                                | Linting exemption tag allowing `LIBDEPS` variables to contain duplicate libdeps on a given library |
| `lint-allow-program-links-private`                      | Linting exemption tag allowing `Program`s to have `PRIVATE_LIBDEPS`                                |

##### The `illegal_cyclic_or_unresolved_dependencies_allowlisted` tag

This tag should not be used anymore because the library dependency graph has been successfully converted to a Directed Acyclic Graph (DAG). Prior to this accomplishment, it was necessary to handle
cycles specifically with platform specific options on the command line.

##### The `init-no-global-side-effects` tag

Adding this flag to a library turns on platform specific compiler flags which will cause the linker to pull in just the symbols it needs. Note that by default, the build is configured to pull in all symbols from libraries because of the use of static initializers, however if a library is known to not have any of these initializers, then this flag can be added for some performance improvement.

#### Linting and linter tags

The libdeps linter features automatically detect certain classes of LIBDEPS usage errors. The libdeps linters are implemented as build-time linting and post-build linting procedures to maintain order in usage of the libdeps tool and the build’s library dependency graph. You will need to comply with the rules enforced by the libdeps linter, and fix issues that it raises when modifying the build scripts. There are exemption tags to prevent the linter from blocking things, however these exemption tags should only be used in extraordinary cases, and with good reason. A goal of the libdeps linter is to drive and maintain the number of exemption tags in use to zero.

##### Exemption Tags

There are a number of existing issues that need to be addressed, but they will be addressed in future tickets. In the meantime, the use of specific strings in the LIBDEPS_TAGS variable can allow the libdeps linter to skip certain issues on given libraries. For example, to have the linter skip enforcement of the lint rule against bidirectional edges for "some_library":

```
env.Library(
    target=’some_library’
    ...
    LIBDEPS_TAGS=[‘lint-allow-bidirectional-edges’]
)
```

#### build-time Libdeps Linter

If there is a build-time issue, the build will fail until it is addressed. This linting feature will be on by default and takes about half a second to complete in a full enterprise build (at the time of writing this), but can be turned off by using the --libdeps-linting=off option on your SCons invocation.

The current rules and there exemptions are listed below:

1. **A 'Program' can not link a non-public dependency, it can only have LIBDEPS links.**

    ###### Example

    ```
    env.Program(
        target=’some_program’,
        ...
        LIBDEPS=[‘lib1’], # OK
        LIBDEPS_PRIVATE=[‘lib2’], # This is a Program, BAD
    )
    ```

    ###### Rationale

    A Program can not be linked into anything else, and there for the transitiveness does not apply. A default value of LIBDEPS was selected for consistency since most Program's were already doing this at the time the rule was created.

    ###### Exemption

    'lint-allow-program-links-private' on the target node

    ######

2. **A 'Node' can only directly link a given library once.**

    ###### Example

    ```
    env.Library(
        target=’some_library’,
        ...
        LIBDEPS=[‘lib1’], # Linked once, OK
        LIBDEPS_PRIVATE=[‘lib1’], # Also linked in LIBDEPS, BAD
        LIBDEPS_INTERFACE=[‘lib2’, 'lib2'], # Linked twice, BAD
    )
    ```

    ###### Rationale

    Libdeps will ignore duplicate links, so this rule is mostly for consistency and neatness in the build scripts.

    ###### Exemption

    'lint-allow-dup-libdeps' on the target node

    ######

3. **A 'Node' which uses LIBDEPS_DEPENDENTS or PROGDEPS_DEPENDENTS can only have LIBDEPS_PRIVATE links.**

    ###### Example

    ```
    env.Library(
        target=’some_library’,
        ...
        LIBDEPS_DEPENDENTS=['lib3'],
        LIBDEPS=[‘lib1’], # LIBDEPS_DEPENDENTS is in use, BAD
        LIBDEPS_PRIVATE=[‘lib2’], # OK
    )
    ```

    ###### Rationale

    The node that the library is using LIBDEPS_DEPENDENTS or PROGDEPS_DEPENDENT to inject its dependency onto should be conditional, therefore there should not be transitiveness for that dependency since it cannot be the source of any resolved symbols.

    ###### Exemption

    'lint-allow-nonprivate-on-deps-dependents' on the target node

    ######

4. **A 'Node' can not link directly to a library that uses LIBDEPS_DEPENDENTS or PROGDEPS_DEPENDENTS.**

    ###### Example

    ```
    env.Library(
        target='other_library',
        ...
        LIBDEPS=['lib1'], # BAD, 'lib1' has LIBDEPS_DEPENDENTS

    env.Library(
        target=’lib1’,
        ...
        LIBDEPS_DEPENDENTS=['lib3'],
    )
    ```

    ###### Rationale

    A library that is using LIBDEPS_DEPENDENTS or PROGDEPS_DEPENDENT should only be used for reverse dependency edges. If a node does need to link directly to a library that does have reverse dependency edges, that indicates the library should be split into two separate libraries, containing its direct dependency content and its conditional reverse dependency content.

    ###### Exemption

    'lint-allow-bidirectional-edges' on the target node

    ######

5. **All libdeps environment vars must be assigned as lists.**

    ###### Example

    ```
    env.Library(
        target='some_library',
        ...
        LIBDEPS='lib1', # not a list, BAD
        LIBDEPS_PRIVATE=['lib2'], # OK
    )
    ```

    ###### Rationale

    Libdeps will handle non-list environment variables, so this is more for consistency and neatness in the build scripts.

    ###### Exemption

    'lint-allow-nonlist-libdeps' on the target node

    ######

6. **Libdeps with the tag 'lint-leaf-node-no-deps' shall not link any libdeps.**

    ###### Example

    ```
    env.Library(
        target='lib2',
        ...
        LIBDEPS_TAGS=[
            'lint-leaf-node-allowed-dep'
        ]
    )

    env.Library(
        target='some_library',
        ...
        LIBDEPS=['lib1'], # BAD, should have no LIBDEPS
        LIBDEPS_PRIVATE=['lib2'], # OK, has exemption tag
        LIBDEPS_TAGS=[
            'lint-leaf-node-no-deps'
        ]
    )
    ```

    ###### Rationale

    The special tag allows certain nodes to be marked and programmatically checked that they remain lead nodes. An example use-case is when we want to make sure certain nodes never link mongodb code.

    ###### Exemption

    'lint-leaf-node-allowed-dep' on the exempted libdep

    ###### Inclusion

    'lint-leaf-node-no-deps' on the target node

    ######

7. **Libdeps with the tag 'lint-no-public-deps' shall not link any libdeps.**

    ###### Example

    ```
    env.Library(
        target='lib2',
        ...
        LIBDEPS_TAGS=[
            'lint-public-dep-allowed'
        ]
    )

    env.Library(
        target='some_library',
        ...
        LIBDEPS=[
            'lib1' # BAD
            'lib2' # OK, has exemption tag
        ],
        LIBDEPS_TAGS=[
            'lint-no-public-deps'
        ]
    )
    ```

    ###### Rationale

    The special tag allows certain nodes to be marked and programmatically checked that they do not link publicly. Some nodes such as mongod_main have special requirements that this programmatically checks.

    ###### Exemption

    'lint-public-dep-allowed' on the exempted libdep

    ###### Inclusion

    'lint-no-public-deps' on the target node

    ######

8. **Libdeps shall be sorted alphabetically in LIBDEPS lists in the SCons files.**

    ###### Example

    ```
    env.Library(
        target='lib2',
        ...
        LIBDEPS=[
            '$BUILD/mongo/db/d', # OK, $ comes before c
            'c', # OK, c comes before s
            'src/a', # BAD, s should be after b
            'b', # BAD, b should be before c
        ]
    )
    ```

    ###### Rationale

    Keeping the SCons files neat and ordered allows for easier Code Review diffs and generally better maintainability.

    ###### Exemption

    'lint-allow-non-alphabetic' on the exempted libdep

    ######

##### The build-time print Option

The libdeps linter also has the `--libdeps-linting=print` option which will perform linting, and instead of failing the build on an issue, just print and continue on. It will also ignore exemption tags, and still print the issue because it will not fail the build. This is a good way to see the entirety of existing issues that are exempted by tags, as well as printing other metrics such as time spent linting.

#### post-build linting and analysis

The dependency graph can be analyzed post-build by leveraging the completeness of the graph to perform more extensive analysis. You will need to install the libdeps requirements file to python when attempting to use the post-build analysis tools:

```
python3 -m poetry install --no-root --sync -E libdeps
```

The command line interface tool (gacli) has a comprehensive help text which will describe the available analysis options and interface. The visualizer tool includes a GUI which displays the available analysis options graphically. These tools will be briefly covered in the following sections.

##### Generating the graph file

To generate the full graph, build the target `generate-libdeps-graph`. This will build all things involving libdeps and construct a graphml file representing the library dependency graph. The graph can be used in the command line interface tool or the visualizer web service tool. The minimal set of required SCons arguments to build the graph file is shown below:

```
python3 buildscripts/scons.py --link-model=dynamic --build-tools=next generate-libdeps-graph --linker=gold --modules=
```

The graph file by default will be generate to `build/opt/libdeps/libdeps.graphml` (where `build/opt` is the `$BUILD_DIR`).

##### General libdeps analyzer API usage

Below is a basic example of usage of the libdeps analyzer API:

```
import libdeps

libdeps_graph = libdeps.graph.load_libdeps_graph('path/to/libdeps.graphml')

list_of_analysis_to_run = [
    libdeps.analyzer.NodeCounter(libdeps_graph),
    libdeps.analyzer.DirectDependencies(libdeps_graph, node='path/to/library'),
]

analysis_results = libdeps.graph.LibdepsGraphAnalysis(list_of_analysis_to_run)
libdeps.analyzer.GaPrettyPrinter(analysis_after_run).print()
```

Walking through this example, first the graph is loaded from file. Then a list of desired Analyzer instances is created. Some example analyzer classes are instantiated in the example above, but there are many others to choose from. Specific Analyzers have different interfaces and should be supplied an argument list corresponding to that analyzer.

_Note:_ The graph file will contain the build dir that the graph data was created with and it expects all node arguments to be relative to the build dir. If you are using the libdeps module generically in some app, you can extract the build dir from the libdeps graph and append it to any generic library path.

Once the list of analyzers is created, they can be used to create a LibdepsGraphAnalysis instance, which will upon instantiation, run the analysis list provided. Once the instance is created, it contains the results, and optionally can be fed into different printer classes. In this case, a human readable format printer called GaPrettyPrinter is used to print to the console.

##### Using the gacli tool

The command line interface tool can be used from the command line to run analysis on a given graph. The only required argument is the graph file. The default with no args will run all the counters and linters on the graph. Here is an example output:

```
(venv) Apr.20 02:46 ubuntu[mongo]: python buildscripts/libdeps/gacli.py --graph-file build/cached/libdeps/libdeps.graphml
Loading graph data...Loaded!


Graph built from git hash:
1358cdc6ff0e53e4f4c01ea0e6fcf544fa7e1672

Graph Schema version:
2

Build invocation:
"/home/ubuntu/venv/bin/python" "buildscripts/scons.py" "--variables-files=etc/scons/mongodbtoolchain_stable_gcc.vars" "--cache=all" "--cache-dir=/home/ubuntu/scons-cache" "--link-model=dynamic" "--build-tools=next" "ICECC=icecc" "CCACHE=ccache" "-j200" "--cache-signature-mode=validate" "--cache-debug=-" "generate-libdeps-graph"

Nodes in Graph: 867
Edges in Graph: 90706
Direct Edges in Graph: 5948
Transitive Edges in Graph: 84758
Direct Public Edges in Graph: 3483
Public Edges in Graph: 88241
Private Edges in Graph: 2440
Interface Edges in Graph: 25
Shim Nodes in Graph: 20
Program Nodes in Graph: 136
Library Nodes in Graph: 731

LibdepsLinter: PUBLIC libdeps that could be PRIVATE: 0
```

Use the `--help` option to see detailing information about all the available options.

##### Using the graph visualizer Tool

The graph visualizer tools starts up a web service to provide a frontend GUI for navigating and examining the graph files. The Visualizer uses a Python Flask backend and React/Redux Javascript frontend.

For installing the dependencies for the frontend, you will need node >= 12.0.0 and npm installed and in the PATH. To install the dependencies navigate to directory where package.json lives, and run:

```
cd buildscripts/libdeps/graph_visualizer_web_stack && npm install
```

Alternatively if you are on linux, you can use the setup_node_env.sh script to automatically download node 12 and npm, setup the local environment and install the dependencies. Run the command:

```
source buildscripts/libdeps/graph_visualizer_web_stack/setup_node_env.sh install
```

Assuming you are on a remote workstation and using defaults, you will need to make ssh tunnels to the web service to access the service in your local browser. The frontend and backend both use a port (this case 3000 is the frontend and 5000 is the backend), and the default host is localhost, so you will need to open two tunnels so the frontend running in your local web browser can communicate with the backend. If you are using the default host and port the tunnel command will look like this:

```
ssh -L 3000:localhost:3000 -L 5000:localhost:5000 ubuntu@workstation.hostname
```

Next we need to start the web service. It will require you to pass a directory where it will search for `.graphml` files which contain the graph data for various commits:

```
python3 buildscripts/libdeps/graph_visualizer.py --graphml-dir build/opt/libdeps
```

The script will launch the backend and then build the optimized production frontend and launch it. You can supply the `--debug` argument to work in development load which starts up much faster and allows real time updates as files are modified, with a small cost to performance on the frontend. Other options allow more configuration and can be viewed in the `--help` text.

After the server has started up, it should notify you via the terminal that you can access it at http://localhost:3000 locally in your browser.

## Build system configuration

### SCons configuration

#### Frequently used flags and variables

### MongoDB build configuration

#### Frequently used flags and variables

##### `MONGO_GIT_HASH`

The `MONGO_GIT_HASH` SCons variable controls the value of the git hash
which will be interpolated into the build to identify the commit
currently being built. If not overridden, this defaults to the git
hash of the current commit.

##### `MONGO_VERSION`

The `MONGO_VERSION` SCons variable controls the value which will be
interpolated into the build to identify the version of the software
currently being built. If not overridden, this defaults to the result
of `git describe`, which will use the local tags to derive a version.

### Targets and Aliases

## Build artifacts and installation

### Hygienic builds

### AutoInstall

### AutoArchive

## MongoDB SCons style guide

### Sconscript Formatting Guidelines

#### Vertical list style

#### Alphabetize everything

### `Environment` Isolation

### Declaring Targets (`Program`, `Library`, and `CppUnitTest`)

### Invoking external tools correctly with `Command`s

### Customizing an `Environment` for a target

### Invoking subordinate `SConscript`s

#### `Import`s and `Export`s

### A Model `SConscript` with Comments
