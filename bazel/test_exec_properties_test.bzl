load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":test_exec_properties.bzl", "POOLS_ARM", "POOLS_X86", "testonly_helpers")

# --- _parse_cpu_tag ---

def _parse_cpu_tag_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, None, testonly_helpers.parse_cpu_tag([]))
    asserts.equals(env, None, testonly_helpers.parse_cpu_tag(["foo", "bar"]))
    asserts.equals(env, 2, testonly_helpers.parse_cpu_tag(["cpu:2"]))
    asserts.equals(env, 2, testonly_helpers.parse_cpu_tag(["resources:cpu:2"]))
    asserts.equals(env, 2, testonly_helpers.parse_cpu_tag(["foo", "cpu:2", "bar"]))
    asserts.equals(env, None, testonly_helpers.parse_cpu_tag(["cpu:abc"]))
    asserts.equals(env, None, testonly_helpers.parse_cpu_tag(["memory:7168"]))

    return unittest.end(env)

_parse_cpu_tag_test = unittest.make(_parse_cpu_tag_test_impl)

# --- _parse_memory_tag ---

def _parse_memory_tag_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, None, testonly_helpers.parse_memory_tag([]))
    asserts.equals(env, None, testonly_helpers.parse_memory_tag(["foo", "bar"]))
    asserts.equals(env, 7168, testonly_helpers.parse_memory_tag(["memory:7168"]))
    asserts.equals(env, 7168, testonly_helpers.parse_memory_tag(["resources:memory:7168"]))
    asserts.equals(env, 14336, testonly_helpers.parse_memory_tag(["foo", "memory:14336", "bar"]))
    asserts.equals(env, None, testonly_helpers.parse_memory_tag(["memory:7gb"]))  # gb suffix not valid
    asserts.equals(env, None, testonly_helpers.parse_memory_tag(["memory:abc"]))  # non-digit value
    asserts.equals(env, None, testonly_helpers.parse_memory_tag(["cpu:2"]))

    return unittest.end(env)

_parse_memory_tag_test = unittest.make(_parse_memory_tag_test_impl)

# --- _choose_pool (x86) ---

def _choose_pool_x86_test_impl(ctx):
    env = unittest.begin(ctx)

    # No memory constraint — pick by CPU only.
    asserts.equals(env, "x86_64", testonly_helpers.choose_pool(POOLS_X86, 1, 0))
    asserts.equals(env, "large_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 2, 0))

    # Memory constraint drives selection beyond the base pool.
    asserts.equals(env, "large_mem_x86_64", testonly_helpers.choose_pool(POOLS_X86, 1, 4096))  # 4 GB
    asserts.equals(env, "large_mem_x86_64", testonly_helpers.choose_pool(POOLS_X86, 1, 7168))  # 7 GB
    asserts.equals(env, "high_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 2, 14336))  # 14 GB

    # Both constraints must be satisfied simultaneously.
    asserts.equals(env, "large_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 2, 7168))

    return unittest.end(env)

_choose_pool_x86_test = unittest.make(_choose_pool_x86_test_impl)

# --- _choose_pool (arm) ---

def _choose_pool_arm_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, "default", testonly_helpers.choose_pool(POOLS_ARM, 1, 0))
    asserts.equals(env, "large_memory_2core_arm64", testonly_helpers.choose_pool(POOLS_ARM, 2, 0))
    asserts.equals(env, "large_memory_arm64", testonly_helpers.choose_pool(POOLS_ARM, 1, 7168))  # 7 GB
    asserts.equals(env, "high_mem_2core_arm64", testonly_helpers.choose_pool(POOLS_ARM, 2, 14336))  # 14 GB
    asserts.equals(env, "large_memory_2core_arm64", testonly_helpers.choose_pool(POOLS_ARM, 2, 7168))

    return unittest.end(env)

_choose_pool_arm_test = unittest.make(_choose_pool_arm_test_impl)

# --- suite ---

def test_exec_properties_test_suite(name):
    _parse_cpu_tag_test(name = name + "_parse_cpu_tag")
    _parse_memory_tag_test(name = name + "_parse_memory_tag")
    _choose_pool_x86_test(name = name + "_choose_pool_x86")
    _choose_pool_arm_test(name = name + "_choose_pool_arm")
    native.test_suite(
        name = name,
        tests = [
            ":" + name + "_parse_cpu_tag",
            ":" + name + "_parse_memory_tag",
            ":" + name + "_choose_pool_x86",
            ":" + name + "_choose_pool_arm",
        ],
    )
