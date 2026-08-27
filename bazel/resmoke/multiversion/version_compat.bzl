"""Module extension that computes multiversion peer availability.

Two things are computed:

    LAST_CONTINUOUS_IS_LAST_LTS  (bool), from .bazelrc.target_mongo_version
                                 plus releases.yml
    LAST_PATCH_HAS_GA_RELEASE    (bool), from the repository's git tags

last-continuous is redundant when it resolves to the same FCV as last-lts,
or when it resolves to an EOL version. In either case, dedicated last-continuous
test suites would duplicate last-lts suites exactly and should be skipped.

last-patch has no peer at all on a series that has never had a GA release
(master, whose only in-series tag is an alpha) or on a freshly cut branch whose
HEAD is still the release tag itself. db-contrib-tool then finds nothing to
download and the multiversion_setup target fails, so dedicated last-patch
suites must be skipped rather than built.

Note what is *not* recomputed here: which tag last-patch resolves to. That
selection is fork-distance based and lives in
buildscripts/resmokelib/multiversion/previous_release_tag.py; duplicating it in
Starlark would be a second implementation of the same policy. Availability is a
much weaker question -- `find_previous_release_tag` returns None only when no
candidate tag survives filtering -- so it needs only "does some GA tag of this
series exist that HEAD is not an ancestor of", which is a couple of git
plumbing calls. This mirrors MultiversionService.has_released_patch_version(),
the same strict (GA-only) predicate mongo-task-generator gates suite generation
on.

The extension reads the version files from the workspace and shells out to git
at fetch time; both constants are loaded into
bazel/resmoke/multiversion/BUILD.bazel to set the default values of the
corresponding bool_flags. Because they are only defaults, either can be
overridden on the command line -- which is the escape hatch for the cases the
strict predicate gets wrong for a particular suite (see the note on the disagg
suites in BUILD.bazel).
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

def _is_ga_release_tag(tag, mongo_version):
    """Return True if `tag` is a final release tag of the `mongo_version` series.

    Starlark has no regex, so this open-codes
    previous_release_tag.FINAL_RELEASE_TAG_RE: 'r8.3.1' matches, while
    'r8.3.1-rc0', 'r9.1.0-alpha0' and 'r8.0.13-s8-0' do not.
    """
    if not tag.startswith("r"):
        return False
    parts = tag[1:].split(".")
    if len(parts) != 3:
        return False
    for part in parts:
        if not part.isdigit():
            return False
    return "{}.{}".format(parts[0], parts[1]) == mongo_version

def _compute_last_patch_has_ga_release(repo_ctx, mongo_version_content):
    """Return True if a GA release of the current series precedes HEAD.

    Fails open (True, i.e. nothing is skipped) whenever the question cannot be
    answered -- no git, no repository, unparsable version file -- matching how
    _compute_redundant treats input it cannot read.
    """
    mongo_version = _parse_mongo_version(mongo_version_content)
    git = repo_ctx.which("git")
    if mongo_version == None or git == None:
        return True

    root = str(repo_ctx.workspace_root)

    # The tag list and HEAD are not files Bazel tracks, so an ordinary
    # `repo_ctx.execute` would leave this repo unfetched after a commit or a
    # fetch that moves either. Watching these two makes the common cases
    # (committing, checking out, `git fetch` writing packed-refs) invalidate it.
    # A tag delivered as a loose ref under .git/refs/tags is not covered; that
    # is what the command-line override on the bool_flag is for.
    for ref_file in [".git/HEAD", ".git/packed-refs"]:
        path = repo_ctx.path(root + "/" + ref_file)
        if path.exists:
            repo_ctx.watch(path)

    tags = repo_ctx.execute([
        git,
        "-C",
        root,
        "tag",
        "-l",
        "r{}.*".format(mongo_version),
    ])
    if tags.return_code != 0:
        return True

    for tag in tags.stdout.splitlines():
        tag = tag.strip()
        if not _is_ga_release_tag(tag, mongo_version):
            continue

        # `find_previous_release_tag` never returns a tag at or descending from
        # HEAD, so a tag HEAD is an ancestor of is not a usable peer. Anything
        # else is: it is an earlier GA release of this series.
        is_ancestor = repo_ctx.execute([
            git,
            "-C",
            root,
            "merge-base",
            "--is-ancestor",
            "HEAD",
            tag,
        ])
        if is_ancestor.return_code != 0:
            return True

    return False

# ---------------------------------------------------------------------------
# Repository rule
# ---------------------------------------------------------------------------

def _multiversion_compat_settings_impl(repo_ctx):
    """Generate settings.bzl with the multiversion peer availability constants."""
    mongo_version_content = repo_ctx.read(
        Label("//:.bazelrc.target_mongo_version"),
    )
    releases_content = repo_ctx.read(
        Label("//src/mongo/util/version:releases.yml"),
    )

    redundant = _compute_redundant(mongo_version_content, releases_content)
    last_patch_has_ga_release = _compute_last_patch_has_ga_release(
        repo_ctx,
        mongo_version_content,
    )

    repo_ctx.file(
        "settings.bzl",
        "LAST_CONTINUOUS_IS_LAST_LTS = {}\nLAST_PATCH_HAS_GA_RELEASE = {}\n".format(
            "True" if redundant else "False",
            "True" if last_patch_has_ga_release else "False",
        ),
    )
    repo_ctx.file("BUILD.bazel", "")

_multiversion_compat_settings = repository_rule(
    implementation = _multiversion_compat_settings_impl,
    doc = "Generates multiversion peer availability flags based on the current MONGO_VERSION.",
)

# ---------------------------------------------------------------------------
# Module extension
# ---------------------------------------------------------------------------

def _multiversion_compat_impl(_ctx):
    _multiversion_compat_settings(name = "multiversion_compat_settings")

multiversion_compat = module_extension(
    implementation = _multiversion_compat_impl,
)
