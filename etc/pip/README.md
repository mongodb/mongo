## On requirements (`*-requirements.txt`) files

MongoDB requires multiple pypa projects installed to build and test. To that end, we provide our own
`*-requirements.txt` files for specific domains of use. Inside each requirements file, there are
only include statements for component files. These files are the bare requirements for specific
components of our python environment. This separation allows us to avoid repetition and conflict in
our requirements across components.

For most developers, if you pip-install `dev-requirements.txt`, you have the python requirements to
lint, build, and test MongoDB.

## How to modify a pypa project requirement in a component

The most common edit of our requirements is likely a change to the constraints on a pypa project
that we already use. For example, say that we currently require `pymongo >= 3.0, < 3.6.0` in the
component `core`. You would like to use PyMongo 3.7, so you instead modify the line in
`etc/pip/components/core.req` to read `pymongo >= 3.0, != 3.6.0`.

## How to add a new component (`*.req`) file

Occasionally, we will require a set of pypa projects for an entirely new piece of software in our
repository. This usually implies adding a new component file. For example, say that we need to add
a logging system to both local development and evergreen. This system requires the fictional pypa
project `FooLog`. So we add a file `foolog.req` and require it from both `dev-requirements.txt` and
`evgtest-requirements.txt`. Like the majority of our components, we want it in the toolchain, so we
also add it to `toolchain-requirements.txt`. The workflow will usually look like:

```
$ # Make the component file
$ echo "FooLog" >etc/pip/components/foolog.req
$ # Require the component from the requirements files
$ echo "-r components/foolog.req" >>etc/pip/dev-requirements.txt
$ echo "-r components/foolog.req" >>etc/pip/evgtest-requirements.txt
$ echo "-r components/foolog.req" >>etc/pip/toolchain-requirements.txt
```

## How to add a new requirements (`*-requirements.txt`) file

Rarely, we will have an entirely new domain of requirements that is useful. In this case, we need to
at least make a new requirements file. For example, say we want to make a requirements file for
packaging our code. We would need most of the requirements for `dev-requirements.txt` but the
testing has already been done in our continuous integration. So we create a new file
`package-requirements.txt` and require a smaller subset of components. The new file at
`etc/pip/package-requirements.txt` would look like this:
```
-r components/platform.req
-r components/core.req

-r components/compile.req
-r components/lint.req
```
