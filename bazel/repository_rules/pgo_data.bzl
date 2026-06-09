load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")
load("//bazel/repository_rules:profiling_data.bzl", "DEFAULT_CLANG_PGO_DATA_CHECKSUM", "DEFAULT_CLANG_PGO_DATA_URL", "DEFAULT_GCC_PGO_DATA_CHECKSUM", "DEFAULT_GCC_PGO_DATA_URL")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain_version_v5.bzl", "TOOLCHAIN_MAP_V5")

def llvm_profdata_toolchain(repository_ctx):
    arch = ARCH_NORMALIZE_MAP.get(repository_ctx.os.arch)
    toolchain_key = "amazon_linux_2023_{}".format(arch)
    if toolchain_key not in TOOLCHAIN_MAP_V5:
        fail("No llvm-profdata toolchain available for host architecture: " + repository_ctx.os.arch)
    return TOOLCHAIN_MAP_V5[toolchain_key]

# This is used so we can tell when the build created new pgo files vs. using ones from stored url
CREATED_FILEGROUP = """
filegroup(
    name = "created_clang_pgo_files",
    srcs = glob(["**/*.profdata"]),
)

filegroup(
    name = "created_gcc_pgo_files",
    srcs = glob(["**/*.gcda"]),
)
"""

EMPTY_CREATED_FILEGROUP = """
filegroup(
    name = "created_clang_pgo_files",
    srcs = [],
    target_compatible_with = ["@platforms//:incompatible"],
)

filegroup(
    name = "created_gcc_pgo_files",
    srcs = [],
    target_compatible_with = ["@platforms//:incompatible"],
)
"""

# recursion and while loops are not allowed, we can only count files to a certain depth
def get_all_files(root_path, depth):
    files = []
    root_paths = [root_path]
    for _ in range(depth):
        if len(root_paths) == 0:
            break
        new_root_paths = []
        for root_path in root_paths:
            for path in root_path.readdir():
                if path.is_dir:
                    new_root_paths.append(path)
                else:
                    files.append(path)
        root_paths = new_root_paths
    return files

def _setup_pgo_data(repository_ctx):
    # This potentially contains multiple urls, separated by a | eg. url1|url2|url3
    pgo_urls_env = repository_ctx.os.environ.get("pgo_profile_url", None)
    clang_profdata_filename = "clang_pgo.profdata"
    created_files = EMPTY_CREATED_FILEGROUP

    # We should be using the default pgo data because we are not being passed pgo data
    if pgo_urls_env == None:
        repository_ctx.download(DEFAULT_CLANG_PGO_DATA_URL, clang_profdata_filename, sha256 = DEFAULT_CLANG_PGO_DATA_CHECKSUM)
        repository_ctx.download_and_extract(DEFAULT_GCC_PGO_DATA_URL, "gcc_pgo", sha256 = DEFAULT_GCC_PGO_DATA_CHECKSUM)

        # This is mainly used for the pgo training pipeline
    else:
        # 4 main scenarios
        # 1. They are passing us a single clang profdata file, just download the file
        # 2. They are passing us a single gcc .tgz with gcda files inside, just download and extract
        # 3. They are passing us one or more clang unprocessed .tgz files with .profraw files inside
        # the .profraw files need to be merged using llvm-profdata into a single .profdata file
        # 4. They are passing us multiple gcc .tgz with gcda files inside, they need to be merged
        # but this is currently unsupported by us

        pgo_urls = pgo_urls_env.split("|")
        created_files = CREATED_FILEGROUP

        # They are passing us a single clang profdata file, just download the file
        if len(pgo_urls) == 1 and pgo_urls[0].endswith(".profdata"):
            print("Downloading single clang profdata file for pgo: " + pgo_urls[0])
            repository_ctx.download(pgo_urls_env, clang_profdata_filename)
        else:
            url_num = 0
            print("Downloading and extracting multiple pgo files.")
            for url in pgo_urls:
                print("Downloading and extracting: " + url)
                repository_ctx.download_and_extract(url, str(url_num))
                url_num += 1

            files = get_all_files(repository_ctx.path("."), 20)
            profraw_files = [file for file in files if file.basename.endswith(".profraw")]

            # This is scenario 3, we need to merge these profraw files
            if len(profraw_files) > 0:
                print("Found profraw files, merging them with llvm-profdata.")
                print(profraw_files)
                llvm_profdata = llvm_profdata_toolchain(repository_ctx)
                repository_ctx.download_and_extract(llvm_profdata["url"], "llvm-profdata", sha256 = llvm_profdata["sha"])
                llvm_path = repository_ctx.path("./llvm-profdata/v5/bin/llvm-profdata")
                arguments = [llvm_path, "merge", "-output=" + clang_profdata_filename] + profraw_files
                result = repository_ctx.execute(arguments)
                print(result.stdout)
                if result.return_code != 0:
                    print(result.stderr)
                    fail("Failed to run llvm-profdata.")

    repository_ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "clang_pgo_files",
    srcs = glob(["**/*.profdata"]),
)

filegroup(
    name = "gcc_pgo_files",
    srcs = glob(["**/*.gcda"]),
)

""" + created_files,
    )

setup_pgo_data = repository_rule(
    implementation = _setup_pgo_data,
    environ = ["pgo_profile_url"],
)
