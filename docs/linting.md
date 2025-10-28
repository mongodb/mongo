# Linting in the MongoDB codebase

## C++ Linters

### `clang-tidy`

The `buildscripts/clang_tidy.py` shell script runs the `clang-tidy` linter. In order to run
`clang-tidy` you must have a compilation database (`compile_commands.json` file).

Ex: `python3 buildscripts/clang_tidy.py`

| Linter       | Configuration File(s) | Help Command        | Documentation                                                                                            |
| ------------ | --------------------- | ------------------- | -------------------------------------------------------------------------------------------------------- |
| `clang-tidy` | `.clang-tidy`         | `clang-tidy --help` | [https://clang.llvm.org/extra/clang-tidy/index.html](https://clang.llvm.org/extra/clang-tidy/index.html) |

### `errorcodes.py`

The `buildscripts/errorcodes.py` script runs a custom error code linter, which verifies that all
assertion codes are distinct. You can see the usage by running the following command:
`buildscripts/errorcodes.py --help`.

Ex: `buildscripts/errorcodes.py`

### `quickmongolint.py`

The `buildscripts/quickmongolint.py` script runs a simple MongoDB C++ linter. You can see the usage
by running the following command: `buildscripts/quickmongolint.py --help`. You can take a look at
`buildscripts/linter/mongolint.py` to better understand the rules for this linter.

Ex: `buildscripts/quickmongolint.py lint`

## Javascript Linters

The `bazel run lint` command runs the `eslint` javascript linter.

| Linter   | Configuration File(s) | Help Command | Documentation                              |
| -------- | --------------------- | ------------ | ------------------------------------------ |
| `eslint` | `.eslint.config.mjs`  |              | [https://eslint.org/](https://eslint.org/) |

## Yaml Linters

The `buildscripts/yamllinters.sh` shell script runs the yaml linters. The supported yaml linters
are: `yamllint` & `evergreen-lint`. `evergreen-lint` is a custom MongoDB linter used specifically
for `evergreen` yaml files.

Ex: `bash buildscripts/yamllinters.sh`

| Linter           | Configuration File(s)     | Help Command                      | Documentation                                                                                  |
| ---------------- | ------------------------- | --------------------------------- | ---------------------------------------------------------------------------------------------- |
| `yamllint`       | `etc/yamllint_config.yml` | `yamllint --help`                 | [https://readthedocs.org/projects/yamllint/](https://readthedocs.org/projects/yamllint/)       |
| `evergreen-lint` | `etc/evergreen_lint.yml`  | `python -m evergreen_lint --help` | [https://github.com/evergreen-ci/config-linter](https://github.com/evergreen-ci/config-linter) |

## Python Linters

The `bazel run lint` command runs all Python linters as well as several other linters in our code base. You can
run auto-remediations via:
`bazel run lint --fix`.

Ex: `bazel run lint`

| Linter | Configuration File(s) | Help Command | Documentation                                                |
| ------ | --------------------- | ------------ | ------------------------------------------------------------ |
| `ruff` | `pyproject.toml`      |              | [https://docs.astral.sh/ruff/](https://docs.astral.sh/ruff/) |
