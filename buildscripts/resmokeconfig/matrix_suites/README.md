# Matrix Resmoke.py Suites

## Summary
Matrix Suites are defined as a combination of explict
suite files (in `buildscripts/resmokeconfig/suites` by default)
and a set of "overrides" for specific keys. The intention is
to avoid duplication of suite definitions as much as
possible with the eventual goal of having most suites be
fully composed of reusable sections, similar to how Genny's
workloads are defined as a set of parameterized `PhaseConfig`s.

## Usage
Matrix suites behave like regular suites for all functionality in resmoke.py,
including `list-suites`, `find-suites` and `run --suites=[SUITE]`.

## Writing a matrix suite mapping file.
Matrix suites consist of a mapping, and a set of overrides in
their eponymous directories. When you are done writing the mapping file, you must 
[generate the matrix suite file.](#generating-matrix-suites)

The "mappings" directory contains YAML files that each contain a suite definition.
Each suite definition includes `base_suite`, and a list of
modifiers. There is also an optional `decription` field that will get output
with the local resmoke invocation.

The fields of modifiers are the following:
1. overrides
2. excludes
3. eval

Each modifier field is a dot-delimited-notation representing the file and field of the modification.
All modifier fields must be in a yaml file in the `overrides` directory
For example `encryption.mongodfixture_ese` would reference the `mongodfixture_ese` field
inside of the `encryption.yml` file inside of the `overrides` directory.

### overrides
All fields referenced in the `overrides` section of the mappings file will overwrite the specified
fields in the `base_suite`.
The `overrides` modifier takes precidence over the `excludes` and `eval` modifiers.
The `overrides` list will be processed in order so order can matter if multiple override modifiers
try to overwrite the same field in the base_suite.

### excludes
All fields referenced in the `excludes` section of the mappings file will append to the specified
`exclude` fields in the base suite.
The only two valid options in the referenced modifier field are `exclude_with_any_tags` and 
`exclude_files`. They are appended in the order they are specified in the mappings file.

### eval
All fields referenced in the `eval` section of the mappings file will append to the specified
`config.shell_options.eval` field in the base suite.
They are appended in the order they are specified in the mappings file.

## Generating matrix suites
The generated matrix suites live in the `buildscripts/resmokeconfig/matrix_suites/generated_suites`
directory. These files may be edited for local testing but must remain consistent with the mapping
files. There is a task in the commit queue that enforces this. To generate a new version of these
matrix suites, you may run `python3 ./buildscripts/resmoke.py generate-matrix-suites`. This commands
will overwrite the current generated matrix suites on disk so make sure you do not have any unsaved
changes to these files.

## Validating matrix suites
All matrix suites are validated whenever they are run to ensure that the mapping file and the
generated suite file are in sync. The `resmoke_validation_tests` task in the commit queue also
ensures that the files are validated.

## FAQ
For questions about the user or authorship experience, 
please reach out in #server-testing.
