"""Module extension that computes whether last-continuous is redundant.

last-continuous is redundant when it resolves to the same FCV as last-lts,
or when it resolves to an EOL version. In either case, dedicated last-continuous
test suites would duplicate last-lts suites exactly and should be skipped.

The extension reads .bazelrc.target_mongo_version and releases.yml from the
workspace at analysis time and generates a repository that exports:

    LAST_CONTINUOUS_IS_LAST_LTS  (bool)

This constant is loaded into bazel/resmoke/multiversion/BUILD.bazel to set
the default value of the //bazel/resmoke/multiversion:last_continuous_is_last_lts
bool_flag.
"""

def _parse_mongo_version(content):
    """Extract 'MAJOR.MINOR' from .bazelrc.target_mongo_version content."""
    for line in content.split("\n"):
        if "MONGO_VERSION=" in line:
            v = line.split("MONGO_VERSION=")[1].strip()
            parts = v.split(".")
            if len(parts) >= 2:
                return "{}.{}".format(parts[0], parts[1])
    return None

def _parse_version(v):
    """Return (major, minor) int tuple for a 'MAJOR.MINOR' string."""
    parts = v.split(".")
    return (int(parts[0]), int(parts[1]))

def _version_lt(a, b):
    """Return True if version string a < version string b."""
    pa = _parse_version(a)
    pb = _parse_version(b)
    return pa[0] < pb[0] or (pa[0] == pb[0] and pa[1] < pb[1])

def _parse_releases_yml(content):
    """Parse releases.yml and return (fcvs, lts, eols) as lists of version strings."""
    fcvs = []
    lts = []
    eols = []
    current = None

    for raw_line in content.split("\n"):
        line = raw_line.rstrip()
        stripped = line.strip()

        if not stripped or stripped.startswith("#"):
            continue

        # Top-level key: switches which list we're filling
        if not line.startswith(" ") and not line.startswith("\t"):
            if stripped.startswith("featureCompatibilityVersions:"):
                current = fcvs
            elif stripped.startswith("longTermSupportReleases:"):
                current = lts
            elif stripped.startswith("eolVersions:"):
                current = eols
            else:
                current = None
        elif stripped.startswith("- ") and current != None:
            v = stripped[2:].strip()

            # Drop inline comments (e.g. # 2022-04) before stripping quotes so
            # entries like '"3.0" # 2018-02' don't leave a trailing '"'.
            if " #" in v:
                v = v[:v.index(" #")].strip()
            v = v.strip('"').strip("'")
            if "." in v:
                current.append(v)

    return fcvs, lts, eols

def _last_before(versions, current):
    """Return the highest version in `versions` strictly less than `current`."""
    best = None
    for v in versions:
        if _version_lt(v, current):
            if best == None or _version_lt(best, v):
                best = v
    return best

def _compute_redundant(mongo_version_content, releases_content):
    """Return True if last-continuous is redundant.

    last-continuous is redundant when it resolves to the same FCV as last-lts
    or to an EOL version."""
    mongo_version = _parse_mongo_version(mongo_version_content)
    if mongo_version == None:
        return False

    fcvs, lts, eols = _parse_releases_yml(releases_content)
    eol_set = {v: True for v in eols}

    # Mirror the logic in multiversionconstants.py:
    # Filter the test-only "100.0" placeholder but keep EOL versions so that
    # last_continuous can itself be EOL (matching multiversion_service.py behaviour).
    candidate_fcvs = [v for v in fcvs if v != "100.0"]
    active_lts = [v for v in lts if not eol_set.get(v)]

    last_continuous = _last_before(candidate_fcvs, mongo_version)
    last_lts = _last_before(active_lts, mongo_version)

    if last_continuous == None or last_lts == None:
        return False

    return last_continuous == last_lts or eol_set.get(last_continuous, False)

# ---------------------------------------------------------------------------
# Repository rule
# ---------------------------------------------------------------------------

def _multiversion_compat_settings_impl(repo_ctx):
    """Generate settings.bzl with LAST_CONTINUOUS_IS_LAST_LTS constant."""
    mongo_version_content = repo_ctx.read(
        Label("//:.bazelrc.target_mongo_version"),
    )
    releases_content = repo_ctx.read(
        Label("//src/mongo/util/version:releases.yml"),
    )

    redundant = _compute_redundant(mongo_version_content, releases_content)

    repo_ctx.file(
        "settings.bzl",
        "LAST_CONTINUOUS_IS_LAST_LTS = {}\n".format("True" if redundant else "False"),
    )
    repo_ctx.file("BUILD.bazel", "")

_multiversion_compat_settings = repository_rule(
    implementation = _multiversion_compat_settings_impl,
    doc = "Generates LAST_CONTINUOUS_IS_LAST_LTS based on the current MONGO_VERSION.",
)

# ---------------------------------------------------------------------------
# Module extension
# ---------------------------------------------------------------------------

def _multiversion_compat_impl(_ctx):
    _multiversion_compat_settings(name = "multiversion_compat_settings")

multiversion_compat = module_extension(
    implementation = _multiversion_compat_impl,
)
