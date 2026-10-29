"""Compatibility shim for the `dependency()` API inside `bazel_rules_mongo`.

`bazel_rules_mongo` is loaded from the outer workspace as a `local_repository`
declared by the root module in //MODULE.bazel, so the labels emitted here are
resolved against the *root module's* repo mapping.

This file mirrors //bazel/uv:defs.bzl in the outer workspace so that
sub-workspace BUILD files can `load("//bazel/uv:defs.bzl", "dependency")`
using the same load path (relative to their own workspace root).

Vendoring into another workspace
--------------------------------
`PYPI_HUB` below is the single seam for workspaces that source their python
deps differently: point it at whatever hub repo your workspace generates and
nothing else here needs to change.

This used to be expressed as `repo_mapping = {"@poetry": "@pypi"}` on the
`local_repository` call, which let this file emit an abstract `@poetry//:`
prefix. bzlmod has no per-repo `repo_mapping`, and a module extension's hub
can only be `use_repo`'d under one name, so the indirection moved from the
repo-name layer to this constant.
"""

# The outer workspace's pycross hub. See "Vendoring into another workspace".
PYPI_HUB = "@pypi"

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

    The `PYPI_HUB` prefix names the outer workspace's pycross hub; see the
    module docstring if you are vendoring this package elsewhere.
    """
    _ = group  # buildifier: silence unused-argument
    return PYPI_HUB + "//:" + _normalize(name)
