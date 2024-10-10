import os
import sys

CONSOLIDATED_TARGETS_MAP = {}


def create_consolidated_targets(env):
    global CONSOLIDATED_TARGETS_MAP
    for _, v in CONSOLIDATED_TARGETS_MAP.items():
        kwargs = v["kwargs"]
        kwargs["LIBDEPS"] = sorted(list(set(kwargs["LIBDEPS"])))

        result = v["env"].Program(f"$BUILD_DIR/{v['target']}", v["sources"], **kwargs)
        v["env"].RegisterTest(v["list_alias"], result[0])
        v["env"].Alias(v["alias"], result)
        v["env"].Alias("CONSOLIDATED_TARGET_" + v["target"] + "_ALIAS", result)


def add_to_consolidated_target(env, target, source, kwargs, test_alias, list_alias):
    if not isinstance(target, list):
        target = [target]

    if not isinstance(source, list):
        source = [source]

    global CONSOLIDATED_TARGETS_MAP
    consol_target = kwargs["CONSOLIDATED_TARGET"]
    kwargs["AIB_COMPONENT"] = consol_target + "_AIB"

    build_dir = env.Dir("$BUILD_DIR").path.replace("\\", "/")
    libdeps = [
        os.path.relpath(os.path.join(os.getcwd(), libdep), env.Dir("#").abspath).replace("\\", "/")
        if not libdep.startswith("$BUILD_DIR")
        else libdep
        for libdep in kwargs["LIBDEPS"]
    ]
    libdeps = [
        "$BUILD_DIR/" + libdep[len("src/") :] if libdep.startswith("src/") else libdep
        for libdep in libdeps
    ]
    libdeps = [
        "$BUILD_DIR" + libdep[len(build_dir) :] if libdep.startswith(build_dir) else libdep
        for libdep in libdeps
    ]
    kwargs["LIBDEPS"] = libdeps

    if consol_target not in CONSOLIDATED_TARGETS_MAP:
        CONSOLIDATED_TARGETS_MAP[consol_target] = {
            "env": env,
            "target": consol_target,
            "sources": [
                os.path.relpath(os.path.join(os.getcwd(), s), env.Dir("#").abspath) for s in source
            ],
            "kwargs": kwargs,
            "alias": test_alias,
            "list_alias": list_alias,
        }
    else:
        CONSOLIDATED_TARGETS_MAP[consol_target]["sources"].extend(
            [os.path.relpath(os.path.join(os.getcwd(), s), env.Dir("#").abspath) for s in source]
        )

        for k, v in kwargs.items():
            if k not in ["LIBDEPS", "AIB_COMPONENT", "AIB_COMPONENTS_EXTRA", "CONSOLIDATED_TARGET"]:
                print(f"ERROR: Consolidating target {target[0]} will drop information in {k}")
                sys.exit(1)
            else:
                if isinstance(CONSOLIDATED_TARGETS_MAP[consol_target]["kwargs"][k], list):
                    CONSOLIDATED_TARGETS_MAP[consol_target]["kwargs"][k].extend(v)
                else:
                    CONSOLIDATED_TARGETS_MAP[consol_target]["kwargs"][k] = v

    return env.Alias("CONSOLIDATED_TARGET_" + consol_target + "_ALIAS")


def exists(env):
    return True


def generate(env):
    env.AddMethod(create_consolidated_targets, "CreateConsolidatedTargets")
    env.AddMethod(add_to_consolidated_target, "AddToConsolidatedTarget")
