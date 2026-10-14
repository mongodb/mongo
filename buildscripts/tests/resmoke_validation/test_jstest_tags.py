import glob
import json
import os
import re
import unittest
from collections import defaultdict
from typing import Optional

from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_on_by_default
from buildscripts.resmokelib.multiversionconstants import (
    REQUIRES_FCV_TAG,
    REQUIRES_FCV_TAG_LATEST,
    REQUIRES_FCV_TAGS_LESS_THAN_LATEST,
    is_explicit_multiversion_test,
)
from buildscripts.resmokelib.utils import jscomment

# Tests that already carried an always-excluded requires_fcv tag when this rule was added. They
# are not running in any suite. Do not add to this list.
# TODO(SERVER-128742): Remove these tags and make the tests pass in the multiversion suites.
EXPLICIT_MULTIVERSION_FCV_TAG_ALLOWLIST = {
    "jstests/multiVersion/genericBinVersion/server-catalog-and-routing/replica_set_to_csrs_promotion_startup_flag.js",
    "jstests/multiVersion/genericChangeStreams/change_stream_v2_fcv_upgrade_downgrade.js",
    "jstests/multiVersion/genericChangeStreams/change_streams_timeseries_fcv_upgrade_downgrade.js",
    "jstests/multiVersion/genericChangeStreams/set_feature_compatibility_version_triggers_change_streams_retargeting.js",
    "jstests/multiVersion/genericSetFCVUsage/fcv_core/set_fcv_automatic_dryRun.js",
    "jstests/multiVersion/genericSetFCVUsage/fcv_core/set_fcv_dry_run_mode.js",
    "jstests/multiVersion/genericSetFCVUsage/migration_pending_recovery_drained_on_auth_shards_fcv_transition.js",
    "jstests/multiVersion/genericSetFCVUsage/priority_port_downgrade_checks.js",
}


class JstestTagRule:
    def __init__(self, failure_message):
        self.failure_message = failure_message
        self.failures = defaultdict(list)

    def check(self, file: str, tag: str) -> None:
        if self._tag_failed(file, tag):
            self.failures[file].append(tag)

    def _tag_failed(self, file: str, tag: str) -> bool:
        raise NotImplementedError()

    def make_failure_message(self) -> Optional[str]:
        if self.failures:
            pretty_failures = json.dumps(self.failures, indent=4)
            return f"{self.failure_message}:\n{pretty_failures}"
        return None


class FeatureFlagIncompatibleTagRule(JstestTagRule):
    def __init__(self):
        super().__init__(
            failure_message="The following tags are not allowed for feature flags that default to true"
        )
        self.disallowed_tags = {
            f"{flag}_incompatible" for flag in get_all_feature_flags_turned_on_by_default()
        }

    def _tag_failed(self, file: str, tag: str) -> bool:
        return tag in self.disallowed_tags


class RequiresFcvTagRule(JstestTagRule):
    def __init__(self):
        super().__init__(
            failure_message="The following tags reference FCV version that is not available"
        )
        self.allowed_tags = [*REQUIRES_FCV_TAGS_LESS_THAN_LATEST, REQUIRES_FCV_TAG_LATEST]

    def _tag_failed(self, file: str, tag: str) -> bool:
        return tag.startswith("requires_fcv_") and tag not in self.allowed_tags


class ExplicitMultiversionRequiresFcvTagRule(JstestTagRule):
    def __init__(self):
        super().__init__(
            failure_message=(
                "The following tests never run anywhere. They only run in the explicit "
                "multiversion suites, which Evergreen always invokes with "
                f"`--excludeWithAnyTags={REQUIRES_FCV_TAG}`. Remove the tag; to require newer "
                "binaries, check the binary version inside the test instead"
            )
        )
        self.disallowed_tags = set(REQUIRES_FCV_TAG.split(","))

    def _tag_failed(self, file: str, tag: str) -> bool:
        return (
            tag in self.disallowed_tags
            and is_explicit_multiversion_test(file)
            and file not in EXPLICIT_MULTIVERSION_FCV_TAG_ALLOWLIST
        )


class JstestFileRule:
    """A rule checked once per file, rather than once per tag."""

    def __init__(self, failure_message):
        self.failure_message = failure_message
        self.failures = defaultdict(list)

    def check(self, file: str, tags: list[str]) -> None:
        raise NotImplementedError()

    def make_failure_message(self) -> Optional[str]:
        if self.failures:
            pretty_failures = json.dumps(self.failures, indent=4)
            return f"{self.failure_message}:\n{pretty_failures}"
        return None


# The database a jstest talks to through the implicit global `db` when a suite does not
# override it. See parseDbHost() in src/mongo/shell/shell_utils.cpp.
DEFAULT_DB_NAME = "test"

DROPS_DEFAULT_DATABASE_TAG = "drops_default_database"

_LINE_COMMENT_RE = re.compile(r"//[^\n]*")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)

# `<recv>.dropDatabase(` or `<recv>.runCommand({dropDatabase:`.
_DROP_CALL_RE = re.compile(
    r"(?P<recv>[A-Za-z_$][\w$]*)\s*\.\s*"
    r"(?:dropDatabase\s*\(|runCommand\s*\(\s*\{\s*dropDatabase\s*:)"
)
# The same, called straight off a `getSiblingDB(...)` rather than through a variable.
_INLINE_DROP_RE = re.compile(
    r"get(?:SiblingDB|Database|DB)\s*\(\s*(?P<arg>[^()]*?)\s*\)\s*\.\s*"
    r"(?:dropDatabase\s*\(|runCommand\s*\(\s*\{\s*dropDatabase\s*:)"
)
# A `db` handle obtained by name, e.g. `const foo = db.getSiblingDB("bar")`.
_DB_HANDLE_RE = re.compile(
    r"(?:const|let|var)\s+(?P<var>[A-Za-z_$][\w$]*)\s*=\s*[\w.$]*\s*"
    r"get(?:SiblingDB|Database|DB)\s*\(\s*(?P<arg>[^()]*?)\s*\)"
)
_STRING_CONST_RE = re.compile(r"(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*[\"']([^\"']*)[\"']")
# A parameter list declaring `db` as a plain item.
_DB_PARAMS = r"\((?:[^()]*,)?\s*db\s*(?:,[^()]*)?\)"
# Something that binds the name `db`, shadowing the global until the enclosing block closes: a
# declaration, a `for (const db of ...)` header, or a parameter of a function, method, catch
# clause, or arrow whose body follows in braces.
_DB_BINDING_RE = re.compile(
    r"(?:const|let|var)\s+db\s*=|"
    r"\bfor\s*\(\s*(?:const|let|var)\s+db\b[^()]*\)\s*\{|"
    # function f(db) {, run(db) {, catch (db) {. Not if (db) { and friends.
    rf"(?<![\w$])(?!(?:if|while|switch|for|with)\b)[\w$]+\s*{_DB_PARAMS}\s*\{{|"
    rf"{_DB_PARAMS}\s*=>\s*\{{|"
    r"(?<![\w$])db\s*=>\s*\{"
)


def _blank_comments(source: str) -> str:
    """Blank out comments, preserving offsets so brace depth and line numbers stay accurate."""
    for pattern in (_BLOCK_COMMENT_RE, _LINE_COMMENT_RE):
        source = pattern.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), source)
    return source


def _brace_depths(source: str) -> list[int]:
    """Return the brace depth at each offset, ignoring braces inside string literals."""
    depth = 0
    depths = []
    quote = None
    for index, char in enumerate(source):
        if quote is not None:
            if char == quote and source[index - 1] != "\\":
                quote = None
        elif char in "\"'`":
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        depths.append(depth)
    return depths


def _db_shadowed_at(source: str, depths: list[int], offset: int) -> bool:
    """Whether `db` at 'offset' is shadowed by a local binding rather than the implicit global.

    A binding counts if it takes effect before 'offset' and the brace depth never falls below
    the binding's own depth in between, meaning the block it belongs to is still open.
    """
    for match in _DB_BINDING_RE.finditer(source, 0, offset):
        anchor = match.end() - 1
        if min(depths[anchor:offset]) >= depths[anchor]:
            return True
    return False


def _resolve_db_name(arg: str, string_consts: dict) -> Optional[str]:
    """Resolve a getSiblingDB() argument to a literal database name."""
    arg = arg.strip()
    if len(arg) > 1 and arg[0] in "\"'" and arg[-1] == arg[0]:
        return arg[1:-1]
    return string_consts.get(arg)


def drops_default_database(source: str) -> list[int]:
    """Return the lines on which 'source' drops the suite's default database.

    Only syntactically obvious cases are reported. A test reaching the default database through
    indirection may not be flagged. The best-effort check exists to catch the common patterns,
    not to prove its absence.
    """
    source = _blank_comments(source)
    if "dropDatabase" not in source:
        return []

    string_consts = dict(_STRING_CONST_RE.findall(source))
    default_db_aliases = {
        match.group("var")
        for match in _DB_HANDLE_RE.finditer(source)
        if _resolve_db_name(match.group("arg"), string_consts) == DEFAULT_DB_NAME
    }
    depths = _brace_depths(source)

    lines = set()
    for match in _DROP_CALL_RE.finditer(source):
        receiver = match.group("recv")
        if receiver in default_db_aliases or (
            receiver == "db" and not _db_shadowed_at(source, depths, match.start())
        ):
            lines.add(source[: match.start()].count("\n") + 1)

    for match in _INLINE_DROP_RE.finditer(source):
        if _resolve_db_name(match.group("arg"), string_consts) == DEFAULT_DB_NAME:
            lines.add(source[: match.start()].count("\n") + 1)

    return sorted(lines)


class DropsDefaultDatabaseTagRule(JstestFileRule):
    """Require tests that drop the default database to be tagged with drops_default_database.

    jstests/core tests run as passthroughs, sharing one fixture and one default database. A
    test that drops that database throws away whatever was wrote beforehand. This prevents
    the calling suite from accumulating state across the whole run. This is a major problem
    for the disagg_format_snapshot task which aims to accumulate the cluster state to later
    take a snapshot. Tagging them lets such suites exclude them by tag.

    Dropping a database the test created for itself, via db.getSiblingDB("<something else>"),
    is not affected and needs no tag.
    """

    # Only passthrough tests share a default database with their neighbours. Tests elsewhere
    # start their own fixture, so dropping the default database there harms nothing. The format
    # dataset workloads run alongside jstests/core in the disagg_format_snapshot passthrough.
    ROOTS = (
        "jstests/core/",
        "src/mongo/db/modules/atlas/jstests/disagg_storage/format_snapshot/workloads/",
    )

    def __init__(self):
        super().__init__(
            failure_message=(
                "The following tests drop the default database on the line(s) shown. That "
                "deletes data written by every test that ran before them in a passthrough. Add "
                f"the '{DROPS_DEFAULT_DATABASE_TAG}' tag, or, preferably, switch the test to a "
                'database of its own with db.getSiblingDB("<test-specific-name>")'
            )
        )

    def check(self, file: str, tags: list[str]) -> None:
        if not file.startswith(self.ROOTS) or DROPS_DEFAULT_DATABASE_TAG in tags:
            return
        with open(file, encoding="utf-8") as fp:
            lines = drops_default_database(fp.read())
        if lines:
            self.failures[file] = lines


class TestDropsDefaultDatabase(unittest.TestCase):
    def assert_flagged(self, source: str):
        self.assertTrue(drops_default_database(source), f"should have been flagged:\n{source}")

    def assert_not_flagged(self, source: str):
        self.assertFalse(drops_default_database(source), f"should not have been flagged:\n{source}")

    def test_implicit_global_db(self):
        self.assert_flagged("db.dropDatabase();")
        self.assert_flagged("assert.commandWorked(db.runCommand({dropDatabase: 1}));")

    def test_default_db_by_name(self):
        self.assert_flagged('db.getSiblingDB("test").dropDatabase();')
        self.assert_flagged('const d = db.getSiblingDB("test");\nd.dropDatabase();')
        self.assert_flagged('const name = "test";\ndb.getSiblingDB(name).dropDatabase();')
        self.assert_flagged(
            'const name = "test";\n'
            "const d = session.getDatabase(name);\n"
            "d.runCommand({dropDatabase: 1});"
        )

    def test_own_database_is_allowed(self):
        self.assert_not_flagged('db.getSiblingDB("mine").dropDatabase();')
        self.assert_not_flagged('const d = db.getSiblingDB("mine");\nd.dropDatabase();')
        self.assert_not_flagged(
            "const d = db.getSiblingDB(jsTestName());\nassert.commandWorked(d.dropDatabase());"
        )

    def test_db_parameter_shadows_only_inside_function(self):
        # The parameter shadows the global, so this drops the caller's database, not ours.
        self.assert_not_flagged("function f(db) {\n  db.dropDatabase();\n}\nf(other);")
        # ... but a top-level call in the same file still hits the global.
        self.assert_flagged("function f(db) {\n  db.foo.insert({});\n}\ndb.dropDatabase();")
        # A drop nested in a function is still the global when nothing shadows `db`.
        self.assert_flagged("function setup() {\n  db.dropDatabase();\n}\nsetup();")

    def test_db_shadowed_by_another_database(self):
        self.assert_not_flagged('var db = db.getSiblingDB("mine");\ndb.dropDatabase();')

    def test_db_parameter_elsewhere_does_not_hide_a_drop(self):
        # A helper taking `db` shadows the global only inside its own body. A drop in a
        # different, non-shadowing function still hits the default database.
        self.assert_flagged(
            "function helper(db) {\n  db.foo.insert({});\n}\n"
            "function setup() {\n  db.dropDatabase();\n}\nsetup();"
        )
        # ... while the drop inside the shadowing helper stays unflagged.
        self.assert_not_flagged(
            "function helper(db) {\n  db.dropDatabase();\n}\n"
            "function setup() {\n  db.foo.insert({});\n}\nsetup();"
        )
        # Only the unshadowed drop is reported, by its own line.
        self.assertEqual(
            [5],
            drops_default_database(
                "function helper(db) {\n  db.dropDatabase();\n}\n"
                "function setup() {\n  db.dropDatabase();\n}\nsetup();"
            ),
        )

    def test_block_local_db_does_not_hide_a_drop_outside_its_block(self):
        self.assert_flagged(
            'function f() {\n  const db = conn.getSiblingDB("mine");\n  db.coll.insert({});\n}\n'
            "db.dropDatabase();"
        )
        # Inside the block the local binding is the one dropped, so this one is fine.
        self.assert_not_flagged(
            'function f() {\n  const db = conn.getSiblingDB("mine");\n  db.dropDatabase();\n}'
        )

    def test_parameters_shadow_only_their_own_body(self):
        self.assert_not_flagged("const f = (db) => {\n  db.dropDatabase();\n};")
        self.assert_not_flagged("const f = db => {\n  db.dropDatabase();\n};")
        self.assert_not_flagged("const o = {\n  run(db) {\n    db.dropDatabase();\n  },\n};")
        self.assert_not_flagged("const f = function (db) {\n  db.dropDatabase();\n};")
        self.assert_not_flagged("try {\n  f();\n} catch (db) {\n  db.dropDatabase();\n}")
        self.assert_flagged("const f = (db) => db.foo.find();\ndb.dropDatabase();")
        self.assert_not_flagged("for (const db of dbs) {\n  db.dropDatabase();\n}")
        self.assert_flagged("for (const db of dbs) {\n  db.foo.drop();\n}\ndb.dropDatabase();")
        # `if (db) {` is a condition, not a parameter list: nothing is shadowed.
        self.assert_flagged("if (db) {\n  db.dropDatabase();\n}")
        # Neither is a parameter that merely defaults to `db`.
        self.assert_flagged("function f(x = db) {\n  db.dropDatabase();\n}")

    def test_unmodelled_scopes_are_reported_not_hidden(self):
        # A brace-less arrow body is not recognised as a scope, so the drop is reported even
        # though the parameter shadows the global. Such a test needs the tag or braces.
        self.assert_flagged("const f = (db) => db.dropDatabase();")
        self.assert_flagged("dbs.forEach((db) => db.dropDatabase());")
        # Likewise a destructured parameter, or one with a default value.
        self.assert_flagged("const f = ({db}) => {\n  db.dropDatabase();\n};")
        self.assert_flagged("function f(conn, db = conn.getDB('x')) {\n  db.dropDatabase();\n}")

    def test_ignores_comments_and_unrelated_code(self):
        self.assert_not_flagged("// db.dropDatabase();")
        self.assert_not_flagged("/*\n * db.dropDatabase();\n */")
        self.assert_not_flagged('db.coll.drop();\ndb.runCommand({drop: "coll"});')
        self.assert_not_flagged('{resource: {db: "test"}, actions: ["dropDatabase"]}')


class TestJstestTags(unittest.TestCase):
    def test_jstest_tags(self):
        os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
        globs = [
            "src/mongo/db/modules/enterprise/jstests/**/*.js",
            "src/mongo/db/modules/atlas/jstests/**/*.js",
            "jstests/**/*.js",
        ]

        tag_rules = [
            FeatureFlagIncompatibleTagRule(),
            RequiresFcvTagRule(),
            ExplicitMultiversionRequiresFcvTagRule(),
        ]
        file_rules = [
            DropsDefaultDatabaseTagRule(),
        ]

        for pattern in globs:
            for file in glob.glob(pattern, recursive=True):
                tags = jscomment.get_tags(file)
                for tag in tags:
                    for tag_rule in tag_rules:
                        tag_rule.check(file, tag)
                for file_rule in file_rules:
                    file_rule.check(file, tags)

        full_failure_message = ""
        for rule in [*tag_rules, *file_rules]:
            failure_message = rule.make_failure_message()
            if failure_message:
                full_failure_message = f"{full_failure_message}\n{failure_message}"

        if full_failure_message.strip():
            self.fail(full_failure_message.strip())
