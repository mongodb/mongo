"""Script to be invoked by GDB for testing decorable pretty printing."""

import re

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


def test_decorable():
    d1_str = gdb.execute("print d1", to_string=True)
    for pattern in expected_patterns:
        search(pattern, d1_str)

    search(up_pattern, gdb.execute("print up", to_string=True))
    search(set_pattern, gdb.execute("print set_type", to_string=True))
    search(static_member_pattern, gdb.execute("print testClass::static_member", to_string=True))


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


def test_string_map():
    search(r"absl::flat_hash_map.*0 elems", gdb.execute("print emptyMap", to_string=True))
    int_map_results = gdb.execute("print intMap", to_string=True)
    search(r"absl::flat_hash_map.*2 elems", int_map_results)
    search(r'\["a"\] = 1', int_map_results)
    search(r'\["b"\] = 1', int_map_results)
    search(
        r'absl::flat_hash_map.*1 elems.*\{\["a"\] = "a_value"\}',
        gdb.execute("print strMap", to_string=True),
    )
    search(r'absl::flat_hash_set.*1 elems.*\{"a"\}', gdb.execute("print strSet", to_string=True))
    # Non empty Hash, Eq, or Alloc functors should pretty print without issues.
    search(r"absl::flat_hash_set.*0 elems", gdb.execute("print checkNonEmptyHash", to_string=True))
    search(r"absl::flat_hash_set.*0 elems", gdb.execute("print checkNonEmptyEq", to_string=True))
    search(r"absl::flat_hash_set.*0 elems", gdb.execute("print checkNonEmptyAlloc", to_string=True))


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


if __name__ == "__main__":
    try:
        gdb.execute("run")
        gdb.execute("frame function main")
        # TODO(SERVER-110170): Currently bugged in clang
        # test_decorable()
        test_dbname_nss()
        # TODO(SERVER-110170): Currently bugged in clang
        # test_string_map()
        test_boost_optional()
        gdb.write("TEST PASSED\n")
    except Exception as err:
        gdb.write("TEST FAILED -- {!s}\n".format(err))
        gdb.execute("quit 1", to_string=True)
