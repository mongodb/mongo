# NOTE: Order matters for the POOLS_{X86,ARM} dictionaries. They are the order in which the
# pools are searched until one with enough memory for the requested amount is found.
# In Starlark, order is deterministic for iterating through dictionaries.
#
# There is one pool per memory tier and core count scales with memory, so a test's `size` alone
# picks its pool; see SIZE_TO_MEMORY_MB below. `cpus` is recorded for documentation only.
POOLS_X86 = {
    "x86_64": {
        "cpus": 1,
        "memory_gb": 3.5,
    },
    "large_mem_2core_x86_64": {
        "cpus": 2,
        "memory_gb": 7,
    },
    "high_mem_2core_x86_64": {
        "cpus": 2,
        "memory_gb": 14,
    },
}

POOLS_ARM = {
    "test_runner_arm64_1core": {
        "cpus": 1,
        "memory_gb": 3.5,
    },
    "test_runner_arm64_2core": {
        "cpus": 2,
        "memory_gb": 7,
    },
    "test_runner_arm64_4core": {
        "cpus": 4,
        "memory_gb": 14,
    },
}

# Memory (in megabytes) requested for each Bazel test size. This is the only mechanism for sizing a
# test: the smallest pool whose memory capacity is at least this much is selected.
#
# NOTE: Keep in sync with --default_test_resources in .bazelrc, which applies the same sizing to
# local execution.
SIZE_TO_MEMORY_MB = {
    "small": 3584,  # 3.5 GB
    "medium": 3584,  # 3.5 GB
    "large": 7168,  # 7 GB
    "enormous": 14336,  # 14 GB
}

def _size_to_memory_mb(size):
    if size not in SIZE_TO_MEMORY_MB:
        fail("Unknown test size '{size}'. Expected one of: {sizes}.".format(
            size = size,
            sizes = ", ".join(SIZE_TO_MEMORY_MB.keys()),
        ))
    return SIZE_TO_MEMORY_MB[size]

def _choose_pool(pools, memory_mb):
    """Returns the smallest pool with at least `memory_mb` of memory.

    If no pool has that much, the largest one is used, since there is nothing bigger to schedule on.
    """
    largest = None
    for pool, resources in pools.items():
        if memory_mb <= resources["memory_gb"] * 1024:
            return pool
        if largest == None or resources["memory_gb"] > pools[largest]["memory_gb"]:
            largest = pool
    return largest

def test_exec_properties(size):
    """Returns execution properties for selecting the appropriate remote execution pool.

    Args:
        size: The Bazel test size ('small', 'medium', 'large' or 'enormous'), which determines the
              memory request via SIZE_TO_MEMORY_MB, and with it the pool and its core count.

    Returns:
        A select() statement that maps platform configurations to execution properties.
    """
    memory_mb = _size_to_memory_mb(size)

    return select({
        "@platforms//cpu:x86_64": {
            "test.Pool": _choose_pool(POOLS_X86, memory_mb),
        },
        "@platforms//cpu:aarch64": {
            "test.Pool": _choose_pool(POOLS_ARM, memory_mb),
        },
        "//conditions:default": {},
    })

# Exported for testing only — not part of the public API.
testonly_helpers = struct(
    size_to_memory_mb = _size_to_memory_mb,
    choose_pool = _choose_pool,
)
