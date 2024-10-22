# Linting in the MongoDB codebase

## C++ Linters

### `clang-format`

The `buildscripts/clang_format.py` wrapper script runs the `clang-format` linter. You can see the
usage message for the wrapper by running `buildscripts/clang_format.py --help`.

Ex: `buildscripts/clang_format.py lint`

| Linter         | Configuration File(s) | Help Command          | Documentation                                                                                |
| -------------- | --------------------- | --------------------- | -------------------------------------------------------------------------------------------- |
| `clang-format` | `.clang-format`       | `clang-format --help` | [https://clang.llvm.org/docs/ClangFormat.html](https://clang.llvm.org/docs/ClangFormat.html) |

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

The `buildscripts/eslint.py` wrapper script runs the `eslint` javascript linter. You can see the
usage message for the wrapper by running `buildscripts/eslint.py --help`.

Ex: `buildscripts/eslint.py lint`

| Linter   | Configuration File(s)           | Help Command    | Documentation                              |
| -------- | ------------------------------- | --------------- | ------------------------------------------ |
| `eslint` | `.eslintrc.yml` `.eslintignore` | `eslint --help` | [https://eslint.org/](https://eslint.org/) |

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

The `buildscripts/pylinters.py` wrapper script runs the Python linters. You can
see the usage message for the wrapper by running the following command:
`buildscripts/pylinters.py --help`.

Ex: `buildscripts/pylinters.py lint`

| Linter | Configuration File(s) | Help Command  | Documentation                                                |
| ------ | --------------------- | ------------- | ------------------------------------------------------------ |
| `ruff` | `pyproject.toml`      | `ruff --help` | [https://docs.astral.sh/ruff/](https://docs.astral.sh/ruff/) |
