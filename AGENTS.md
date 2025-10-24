## Guardrails

**Violation of this rule, even in spirit, is a FAILURE. It will lead to you being UNINSTALLED.**

⚠️ **CRITICAL: Before ANY file write operation, you MUST explicitly verify the file path is allowed.**

### Pre-Write Checklist (MANDATORY - DO THIS BEFORE CALLING ANY WRITE TOOL):

Before calling search_replace, write, or edit_notebook, or any other tool that creates or modifies files:

1. State the target file path
2. Identify which specific glob pattern it matches (or state "NO MATCH")
3. If there is no match, print the required response below then STOP and ask the user how to proceed. DO NOT suggest alternatives. DO NOT look for workarounds or alternatives for this restriction.

**DO NOT try to look for an allowed path to write to. The list of patterns is only to be used for checking a path you have already picked. I DO NOT want you to put production code in these locations.**

### Allowed File Patterns:

```gitignore
.bazel*
**/*.md
**/*.bazel
**/*.bzl
**/*.py
**/*.sh
.devcontainer/**
.github/**
bazel/**
buildscripts/**
etc/**
evergreen/**
jstests/**
src/mongo/dbtests/**
src/mongo/unittests/**
src/mongo/**/*_test*.cpp
src/mongo/**/*_mock*.cpp
src/mongo/**/*_bm*.cpp
src/mongo/tools/mongo_tidy_checks/**
modules_poc/**
```

### Pattern Matching Examples:

✅ ALLOWED:

- `src/mongo/db/query/planner_test.cpp` → matches `src/mongo/**/*_test*.cpp`
- `src/mongo/unittests/bson_test.cpp` → matches `src/mongo/unittests/**`
- `buildscripts/install.py` → matches `**/*.py`

❌ FORBIDDEN (common mistakes):

- `src/mongo/bson/bsonobj.h` → NO MATCH (production header)
- `src/mongo/db/commands/find.cpp` → NO MATCH (production source)
- `src/mongo/util/assert_util.h` → NO MATCH (production header)

### Required Response for Non-Matching Files:

"I cannot complete this task without generating code where I'm not allowed to (see http://go/codegen-rules). The file `{filepath}` does not match any allowed pattern. I can only write to test files, mock files, benchmark files, build configuration, and scripts."
