load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":test_exec_properties.bzl", "POOLS_ARM", "POOLS_X86", "testonly_helpers")

# --- _size_to_memory_mb ---

def _size_to_memory_mb_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, 3584, testonly_helpers.size_to_memory_mb("small"))
    asserts.equals(env, 3584, testonly_helpers.size_to_memory_mb("medium"))
    asserts.equals(env, 7168, testonly_helpers.size_to_memory_mb("large"))
    asserts.equals(env, 14336, testonly_helpers.size_to_memory_mb("enormous"))

    return unittest.end(env)

_size_to_memory_mb_test = unittest.make(_size_to_memory_mb_test_impl)

# --- _choose_pool (x86) ---

def _choose_pool_x86_test_impl(ctx):
    env = unittest.begin(ctx)

    # One pool per memory tier; each size maps onto exactly one of them.
    asserts.equals(env, "x86_64", testonly_helpers.choose_pool(POOLS_X86, 3584))  # small, medium
    asserts.equals(env, "large_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 7168))  # large
    asserts.equals(env, "high_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 14336))  # enormous

    # Anything in between rounds up to the next tier.
    asserts.equals(env, "x86_64", testonly_helpers.choose_pool(POOLS_X86, 0))
    asserts.equals(env, "large_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 4096))  # 4 GB

    # Memory beyond every pool caps to the largest pool.
    asserts.equals(env, "high_mem_2core_x86_64", testonly_helpers.choose_pool(POOLS_X86, 28672))  # 28 GB

    return unittest.end(env)

_choose_pool_x86_test = unittest.make(_choose_pool_x86_test_impl)

# --- _choose_pool (arm) ---

def _choose_pool_arm_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, "test_runner_arm64_1core", testonly_helpers.choose_pool(POOLS_ARM, 3584))  # small, medium
    asserts.equals(env, "test_runner_arm64_2core", testonly_helpers.choose_pool(POOLS_ARM, 7168))  # large
    asserts.equals(env, "test_runner_arm64_4core", testonly_helpers.choose_pool(POOLS_ARM, 14336))  # enormous

    asserts.equals(env, "test_runner_arm64_1core", testonly_helpers.choose_pool(POOLS_ARM, 0))
    asserts.equals(env, "test_runner_arm64_2core", testonly_helpers.choose_pool(POOLS_ARM, 4096))  # 4 GB

    # Memory beyond every pool caps to the largest pool.
    asserts.equals(env, "test_runner_arm64_4core", testonly_helpers.choose_pool(POOLS_ARM, 28672))  # 28 GB

    return unittest.end(env)

_choose_pool_arm_test = unittest.make(_choose_pool_arm_test_impl)

# --- suite ---

def test_exec_properties_test_suite(name):
    _size_to_memory_mb_test(name = name + "_size_to_memory_mb")
    _choose_pool_x86_test(name = name + "_choose_pool_x86")
    _choose_pool_arm_test(name = name + "_choose_pool_arm")
    native.test_suite(
        name = name,
        tests = [
            ":" + name + "_size_to_memory_mb",
            ":" + name + "_choose_pool_x86",
            ":" + name + "_choose_pool_arm",
        ],
    )
