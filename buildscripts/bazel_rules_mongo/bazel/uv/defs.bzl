"""Compatibility shim for the `dependency()` API inside `bazel_rules_mongo`.

`bazel_rules_mongo` is loaded from the outer workspace as a `local_repository`
in //WORKSPACE.bazel with `repo_mapping = {"@poetry": "@pypi"}`. Under that
mapping, `@poetry` inside this sub-workspace resolves to the pycross-generated
hub `@pypi` in the outer workspace's bzlmod graph.

This file mirrors //bazel/uv:defs.bzl in the outer workspace so that
sub-workspace BUILD files can `load("//bazel/uv:defs.bzl", "dependency")`
using the same load path (relative to their own workspace root).
"""

def _normalize(name):
    """PEP 503 normalization used by rules_pycross's `@pypi//:<name>` labels.

    Starlark has no `while`; the run-collapse loop below is bounded by
    len(out) (a safe upper bound — a run of N hyphens converges in at most
    N passes and usually far fewer, thanks to `replace`'s greedy behavior).
    """
    out = name.lower()
    for ch in ("_", "."):
        out = out.replace(ch, "-")
    for _ in range(len(out)):
        if "--" not in out:
            break
        out = out.replace("--", "-")
    return out

def dependency(name, group = None):
    """Return the Bazel label for a PyPI package locked in the outer workspace's uv.lock.

    Note the `@poetry` prefix — it's the sub-workspace's alias for the outer
    workspace's `@pypi` hub (see the `repo_mapping` on the `local_repository`
    call in the outer //WORKSPACE.bazel).
    """
    _ = group  # buildifier: silence unused-argument
    return "@poetry//:" + _normalize(name)
