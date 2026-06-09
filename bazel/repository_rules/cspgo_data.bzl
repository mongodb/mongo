load("//bazel/repository_rules:pgo_data.bzl", "get_all_files", "llvm_profdata_toolchain")
load(
    "//bazel/repository_rules:profiling_data.bzl",
    "DEFAULT_CLANG_CSPGO_DATA_CHECKSUM",
    "DEFAULT_CLANG_CSPGO_DATA_URL",
    "DEFAULT_CLANG_PGO_DATA_CHECKSUM",
    "DEFAULT_CLANG_PGO_DATA_URL",
)

# This is used so we can tell when the build created new cspgo files vs. using ones from stored url
CREATED_FILEGROUP = """
filegroup(
    name = "created_clang_cspgo_files",
    srcs = glob(["**/*.profdata"]),
)
"""

EMPTY_CREATED_FILEGROUP = """
filegroup(
    name = "created_clang_cspgo_files",
    srcs = [],
    target_compatible_with = ["@platforms//:incompatible"],
)
"""

def _download_urls(repository_ctx, urls_env, subdir_prefix):
    """Download a pipe-separated URL string into numbered subdirs. .profdata URLs are
    downloaded in-place; everything else is treated as an archive and extracted."""
    url_num = 0
    for url in urls_env.split("|"):
        print("Downloading: " + url)
        if url.endswith(".profdata"):
            repository_ctx.download(url, subdir_prefix + str(url_num) + ".profdata")
        else:
            repository_ctx.download_and_extract(url, subdir_prefix + str(url_num))
        url_num += 1

def _setup_cspgo_data(repository_ctx):
    # cspgo_profile_url carries stage-2 context-sensitive data (raw .profraw tarballs or a
    # pre-merged .profdata). pgo_profile_url carries stage-1 PGO data that gets merged with
    # the stage-2 inputs. Both are pipe-separated when multiple files are provided.
    cspgo_urls_env = repository_ctx.os.environ.get("cspgo_profile_url", None)
    pgo_urls_env = repository_ctx.os.environ.get("pgo_profile_url", None)
    cspgo_profdata_filename = "clang_cspgo.profdata"
    created_files = EMPTY_CREATED_FILEGROUP

    # CSPGO is clang-only - GCC has no -fcs-profile-generate equivalent.

    # No override - fall back to the published default (if populated).
    if cspgo_urls_env == None:
        # Allow an unset default so the repo still resolves before a profile has been
        # published. Builds that enable cspgo_profile_use will fail on the empty
        # filegroup below with a clear missing-input error.
        if DEFAULT_CLANG_CSPGO_DATA_URL != "":
            repository_ctx.download(DEFAULT_CLANG_CSPGO_DATA_URL, cspgo_profdata_filename, sha256 = DEFAULT_CLANG_CSPGO_DATA_CHECKSUM)

        # Training pipeline.
    else:
        # 2 scenarios for the cspgo url:
        # 1. A single pre-merged clang cspgo .profdata - already contains PGO + CSPGO data,
        #    just download it.
        # 2. One or more raw .profraw tarballs. Merge them with stage-1 PGO data from
        #    pgo_profile_url (or DEFAULT_CLANG_PGO_DATA_URL if pgo_profile_url is unset)
        #    into a single combined profdata.

        cspgo_urls = cspgo_urls_env.split("|")
        created_files = CREATED_FILEGROUP

        if len(cspgo_urls) == 1 and cspgo_urls[0].endswith(".profdata"):
            print("Downloading single pre-merged clang profdata file for cspgo: " + cspgo_urls[0])
            repository_ctx.download(cspgo_urls_env, cspgo_profdata_filename)
        else:
            print("Downloading cspgo raw profile data.")
            _download_urls(repository_ctx, cspgo_urls_env, "cspgo_")

            # Fold stage-1 PGO data into the merge. Prefer pgo_profile_url so the user's
            # command-line choice is respected; otherwise fall back to the default URL.
            if pgo_urls_env != None:
                print("Folding stage-1 pgo data from pgo_profile_url into the merge.")
                _download_urls(repository_ctx, pgo_urls_env, "pgo_")
            elif DEFAULT_CLANG_PGO_DATA_URL != "":
                print("Folding default stage-1 pgo data into the merge.")
                repository_ctx.download(DEFAULT_CLANG_PGO_DATA_URL, "pgo_default.profdata", sha256 = DEFAULT_CLANG_PGO_DATA_CHECKSUM)
            else:
                fail("cspgo raw profile merge requires stage-1 pgo data. Set --repo_env=pgo_profile_url=... or populate DEFAULT_CLANG_PGO_DATA_URL.")

            files = get_all_files(repository_ctx.path("."), 20)
            merge_inputs = [file for file in files if file.basename.endswith(".profraw") or file.basename.endswith(".profdata")]

            if len(merge_inputs) > 0:
                print("Merging cspgo + pgo inputs with llvm-profdata:")
                print(merge_inputs)
                llvm_profdata = llvm_profdata_toolchain(repository_ctx)
                repository_ctx.download_and_extract(llvm_profdata["url"], "llvm-profdata", sha256 = llvm_profdata["sha"])
                llvm_path = repository_ctx.path("./llvm-profdata/v5/bin/llvm-profdata")
                arguments = [llvm_path, "merge", "-output=" + cspgo_profdata_filename] + merge_inputs
                result = repository_ctx.execute(arguments)
                print(result.stdout)
                if result.return_code != 0:
                    print(result.stderr)
                    fail("Failed to run llvm-profdata.")

                # clean up pre-merge inputs so they aren't picked up by the glob below
                for file in merge_inputs:
                    repository_ctx.delete(file)

    repository_ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "clang_cspgo_files",
    srcs = glob(["**/*.profdata"]),
)

""" + created_files,
    )

setup_cspgo_data = repository_rule(
    implementation = _setup_cspgo_data,
    environ = ["cspgo_profile_url", "pgo_profile_url"],
)
