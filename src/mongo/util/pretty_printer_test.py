"""Script to be invoked by GDB for testing decorable pretty printing."""

import dataclasses
import enum
import re
import traceback

import gdb

expected_patterns = [
    r"Decorable<MyDecorable\> with 3 elems",
    r"vector of length 3.*\{ *123, *213, *312 *\}",
    r'basic_string.* \= *"hello"',
    r'basic_string.* \= *"world"',
]
up_pattern = r"std::unique_ptr<int\> = \{get\(\) \= 0x[0-9a-fA-F]+\}"
set_pattern = r"std::[__debug::]*set with 4 elements"
static_member_pattern = "128"


def search(pattern, s):
    match = re.search(pattern, s)
    assert match is not None, "Did not find {!s} in {!s}".format(pattern, s)
    return match


def gdbexec(cmd, *args, **kwargs):
    """Like gdb.execute but the to_string argument defaults to True."""
    kwargs.setdefault("to_string", True)
    return gdb.execute(cmd, *args, **kwargs)


def gdbprint(ident, *args, **kwargs):
    return gdbexec(f"print {ident}", *args, **kwargs)


def test_decorable():
    d1_str = gdb.execute("print d1", to_string=True)
    for pattern in expected_patterns:
        search(pattern, d1_str)

    search(up_pattern, gdb.execute("print up", to_string=True))
    search(set_pattern, gdb.execute("print set_type", to_string=True))
    # TODO(SERVER-118950): re-enable when problem relating to debug symbols is solved:
    # `Missing ELF symbol "_ZN9testClass13static_memberE"`.
    # search(static_member_pattern, gdb.execute("print testClass::static_member", to_string=True))


def test_dbname_nss():
    dbname_str = gdb.execute("print dbName", to_string=True)
    search("foo", dbname_str)
    dbname_tid_str = gdb.execute("print dbNameWithTenantId", to_string=True)
    search("6491a2112ef5c818703bf2a7_foo", dbname_tid_str)
    nss_str = gdb.execute("print nss", to_string=True)
    search("foo.ba", nss_str)
    nss_tid_str = gdb.execute("print nssWithTenantId", to_string=True)
    search("6491a2112ef5c818703bf2a7_foo.barbaz", nss_tid_str)
    long_nss_str = gdb.execute("print longNss", to_string=True)
    search("longdatabasenamewithoutsmallstring.longcollection", long_nss_str)
    constexpr_str = gdb.execute("print kConstNs", to_string=True)
    search("constexpr.name", constexpr_str)
    constexpr_str = gdb.execute("print constCopy", to_string=True)
    search("constexpr.name", constexpr_str)


def test_absl_container_printers():
    class CppType(enum.Enum):
        STRING = "string"
        INT = "int"

    def element(i, cpp_type):
        # Returns the ith element of cpp_type as gdb prints it. The test program fills
        # containers with fixed contents: strings are "a", "b", ... "z", "aa", "bb", ... and
        # ints are 0, 1, .... The ith key and the ith value correspond, so a string-to-int
        # map of size N holds only elements like ("a", 0) or ("c", 2) but not ("a", 1).
        match cpp_type:
            case CppType.STRING:
                return f'"{chr(ord("a") + i % 26) * (i // 26 + 1)}"'
            case CppType.INT:
                return str(i)
            case _:
                raise ValueError(f"unsupported cpp type: {cpp_type}")

    @dataclasses.dataclass(frozen=True)
    class ContainerSpec:
        typename: str
        ident: str
        key_type: CppType | None
        value_type: CppType

    class CppContainer:
        def __init__(self, *args, **kwargs):
            self._spec = ContainerSpec(*args, **kwargs)

        def _element_text(self, i):
            # Sets print as {v, ...} and maps as {[k] = v, ...}.
            spec = self._spec
            value = element(i, spec.value_type)
            if spec.key_type is None:
                return value
            return f"[{element(i, spec.key_type)}] = {value}"

        def assert_size_is(self, size):
            t = self._spec.typename
            ident = self._spec.ident
            search(rf"{t}.* {size} elem", gdbprint(ident))

        def assert_contents(self, indices):
            # indices are the elements the container is expected to hold, so [0, 2] means
            # a string-to-int map holds {("a", 0), ("c", 2)}.
            self.assert_size_is(len(indices))
            printed = gdbprint(self._spec.ident)
            for i in indices:
                text = self._element_text(i)
                assert text in printed, f"Did not find {text} in {printed}"

    # Each member of AbslContainerStates, and the elements it holds.
    container_states = [
        ("empty", []),
        ("movedFrom", []),
        ("insert1", [0]),
        ("insert1ThenDelete", []),
        ("insert1ThenClear", []),
        ("insert1ThenDeleteThenInsert", [1]),
        ("insert1ThenClearThenInsert", [1]),
        ("insert2", list(range(2))),
        ("insert3", list(range(3))),
        ("insert3Delete1", list(range(1, 3))),
        ("insert3Delete2", [2]),
        ("insert3DeleteAll", []),
        ("insert3DeleteAllReinsert", [3]),
        ("insert8", list(range(8))),
        ("insert8Delete4", list(range(4, 8))),
        ("insert8Delete4Reinsert", list(range(4, 12))),
        ("insert8DeleteAll", []),
        ("insert8DeleteAllReinsert", [8]),
        ("insert28", list(range(28))),
        ("insert28Delete4", list(range(4, 28))),
        ("insert28Delete4Reinsert", list(range(4, 32))),
    ]

    # Each AbslContainerStates instance in the test program.
    container_specs = [
        ("absl::flat_hash_map", "stringIntMaps", CppType.STRING, CppType.INT),
        ("absl::flat_hash_map", "stringStringMaps", CppType.STRING, CppType.STRING),
        ("absl::flat_hash_map", "intStringMaps", CppType.INT, CppType.STRING),
        ("absl::flat_hash_set", "stringSets", None, CppType.STRING),
        ("absl::flat_hash_map", "intIntMaps", CppType.INT, CppType.INT),
        ("absl::flat_hash_set", "intSets", None, CppType.INT),
        ("absl::node_hash_map", "stringIntNodeMaps", CppType.STRING, CppType.INT),
        ("absl::node_hash_map", "stringStringNodeMaps", CppType.STRING, CppType.STRING),
        ("absl::node_hash_map", "intIntNodeMaps", CppType.INT, CppType.INT),
        ("absl::node_hash_map", "intStringNodeMaps", CppType.INT, CppType.STRING),
        ("absl::node_hash_set", "stringNodeSets", None, CppType.STRING),
        ("absl::node_hash_set", "intNodeSets", None, CppType.INT),
        ("absl::flat_hash_set", "nonEmptyHashSets", None, CppType.STRING),
        ("absl::flat_hash_set", "nonEmptyEqSets", None, CppType.STRING),
        ("absl::flat_hash_set", "nonEmptyAllocSets", None, CppType.STRING),
    ]

    for typename, ident, key_type, value_type in container_specs:
        for member, indices in container_states:
            c = CppContainer(typename, f"{ident}.{member}", key_type, value_type)
            c.assert_contents(indices)


def test_boost_optional():
    optional = get_boost_optional(gdb.parse_and_eval("optTypeNone"))
    assert optional is None, f"optTypeNone was {optional}"

    optional = get_boost_optional(gdb.parse_and_eval("optTypeValue"))
    assert optional is not None, f"optTypeValue was {optional}"
    assert optional == 1, f"optTypeValue was {optional}"

    optional = get_boost_optional(gdb.parse_and_eval("wrappedOptTypeNone"))
    assert optional is None, f"wrappedOptTypeNone was {optional}"

    optional = get_boost_optional(gdb.parse_and_eval("wrappedOptTypeValue"))
    assert optional is not None, f"wrappedOptTypeValue was {optional}"
    assert optional["_i"] == 1, f"wrappedOptTypeValue was {optional}"


def test_bsonobj():
    search(
        r'owned BSONObj.*\{\[x\] = "1", \[sub\] = \{"y": "1"\}\}',
        gdb.execute("print obj", to_string=True),
    )
    search(
        r'unowned BSONObj.*\{\[x\] = "1", \[sub\] = \{"y": "1"\}\}',
        gdb.execute("print unownedObj", to_string=True),
    )


if __name__ == "__main__":
    try:
        gdb.execute("run")
        gdb.execute("frame function main")
        test_decorable()
        test_dbname_nss()
        test_absl_container_printers()
        test_boost_optional()
        test_bsonobj()
        gdb.write("TEST PASSED\n")
    except Exception as err:
        gdb.write(traceback.format_exc())
        gdb.write("TEST FAILED -- {!s}\n".format(err))
        gdb.execute("quit 1", to_string=True)
