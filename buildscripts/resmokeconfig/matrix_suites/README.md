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
including `list-suites`, `find-suites` and `run --suite=[SUITE]`.

## Writing a matrix suite.
Matrix suites consist of a set of mappings, and a set of overrides in
their eponymous directories.

The "mappings" directory contains YAML files that each contain a list of 
suite definitions.
Each suite definition includes `suite_name`, `base_suite`, and a list of
`overrides`. Each `override` is a dot-delimited-notation pointing to a field
in the original suite YAML file that is overridden by a section in a YAML file
in the `overrides` directory with the same name as the file in the `mappings`
directory.

## FAQ
For questions about the user or authorship experience, 
please reach out in #server-testing.
