# NOTE: Order matters for the POOLS_{X86,ARM} dictionaries. They are the order in which the
# pools are searched until one with resources sufficient for the requested amount is found.
# In Starlark, order is deterministic for iterating through dictionaries.
POOLS_X86 = {
    "x86_64": {
        "cpus": 1,
        "memory_gb": 3.5,
    },
    "large_mem_x86_64": {
        "cpus": 1,
        "memory_gb": 7,
    },
    "large_mem_2core_x86_64": {
        "cpus": 2,
        "memory_gb": 7,
    },
}

POOLS_ARM = {
    "default": {
        "cpus": 1,
        "memory_gb": 3.5,
    },
    "large_memory_arm64": {
        "cpus": 1,
        "memory_gb": 7,
    },
    "large_memory_2core_arm64": {
        "cpus": 2,
        "memory_gb": 7,
    },
}

def _parse_cpu_tag(tags):
    for tag in tags:
        # Bazel handles the full 'resources:cpu:2' and the short form 'cpu:2'.
        resource, _, value = tag.removeprefix("resources:").rpartition(":")
        if resource == "cpu" and value.isdigit():
            return int(value)
    return None

def _choose_pool(pools, cpus):
    for pool, resources in pools.items():
        if cpus <= resources["cpus"]:
            return pool
    fail("Requested {cpus} CPUs by tag 'cpu:{cpus}', but there is no remote execution pool that can satisfy this.".format(cpus = cpus))

def test_exec_properties(tags):
    cpus = _parse_cpu_tag(tags)
    if not cpus:
        return {}

    pool_arm = _choose_pool(POOLS_ARM, cpus)
    pool_x86 = _choose_pool(POOLS_X86, cpus)

    return select({
        "@platforms//cpu:x86_64": {
            "Pool": pool_x86,
        },
        "@platforms//cpu:aarch64": {
            "Pool": pool_arm,
        },
        "//conditions:default": {},
    })
