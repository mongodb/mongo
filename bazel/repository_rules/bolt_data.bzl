load("//bazel/repository_rules:pgo_data.bzl", "get_all_files")
load("//bazel/repository_rules:profiling_data.bzl", "DEFAULT_BOLT_DATA_CHECKSUM", "DEFAULT_BOLT_DATA_URL")

# This is used so we can tell when the build created new bolt files vs. using ones from stored url
CREATED_FILEGROUP = """
filegroup(
    name = "created_bolt_fdata",
    srcs = glob(["**/*.fdata"]),
)
"""

EMPTY_CREATED_FILEGROUP = """
filegroup(
    name = "created_bolt_fdata",
    srcs = [],
    target_compatible_with = ["@platforms//:incompatible"],
)
"""

def _setup_bolt_data(repository_ctx):
    bolt_fdata_filename = "bolt.fdata"

    # This potentially contains multiple urls, separated by a | eg. url1|url2|url3
    bolt_profile_urls_env = repository_ctx.os.environ.get("bolt_profile_url", None)

    # This is the binary the bolt data came from
    bolt_binary_env = repository_ctx.os.environ.get("bolt_binary_url", None)

    # Incase you want to bolt a binary instead of the main binary mongod
    bolt_binary_name = repository_ctx.os.environ.get("bolt_binary_name", None)

    created_files = EMPTY_CREATED_FILEGROUP

    # Perf2bolt will use the path to call the perf tool
    path_env = repository_ctx.os.environ.get("PATH", None)
    perf_path_env = str(repository_ctx.path(repository_ctx.attr._perf_binary).dirname) + ":" + path_env

    if bolt_binary_name == None:
        bolt_binary_name = "mongod"

    # We should be using the default bolt data because we are not being passed bolt data
    if bolt_profile_urls_env == None and bolt_binary_env == None:
        repository_ctx.download(DEFAULT_BOLT_DATA_URL, bolt_fdata_filename, sha256 = DEFAULT_BOLT_DATA_CHECKSUM)

        # This is mainly used for the bolt training pipeline
    else:
        # 2 main scenarios
        # 1. They are passing us a single bolt .fdata file, just download the file
        # 2. They are passing us one or more unprocessed .tgz files with .data files inside
        # the .data files need to be turned into fdata files using perf2bolt, which then can be merged
        # into a single fdata file using merge-fdata

        bolt_urls = bolt_profile_urls_env.split("|")
        created_files = CREATED_FILEGROUP

        # They are passing us a single bolt fdata file, just download the file
        if len(bolt_urls) == 1 and bolt_urls[0].endswith(".fdata"):
            print("Downloading single fdata file for bolt: " + bolt_urls[0])
            repository_ctx.download(bolt_profile_urls_env, bolt_fdata_filename)
        else:
            url_num = 0
            print("Downloading and extracting multiple bolt files.")
            for url in bolt_urls:
                print("Downloading and extracting: " + url)
                repository_ctx.download_and_extract(url, str(url_num))
                url_num += 1

            # Download the mongod binary for perf2bolt
            print("Download the mongo binaries from: " + bolt_binary_env)
            repository_ctx.download_and_extract(bolt_binary_env, "binaries")

            files = get_all_files(repository_ctx.path("."), 20)
            data_files = [file for file in files if file.basename.endswith(".data")]
            fdata_files = [file for file in files if file.basename.endswith(".fdata")]
            binary = [file for file in files if file.basename.endswith(bolt_binary_name)][0]

            processed_fdata_files = 0

            # This is scenario 2, we need to turn these data files into fdata files using perf2bolt
            if len(data_files) > 0:
                print("Found data files, turning them into fdata files with perf2bolt.")
                for file in data_files:
                    fdata_file_name = "bolt" + str(processed_fdata_files) + ".fdata"
                    arguments = [repository_ctx.attr._perf2bolt_binary, "-nl", "-p", file, "-o", fdata_file_name, binary]

                    # We execute perf through path so it doesn't get executable permissions normally
                    repository_ctx.execute(["chmod", "+x", repository_ctx.attr._perf_binary])
                    result = repository_ctx.execute(arguments, environment = {"PATH": perf_path_env})
                    print(result.stdout)
                    if result.return_code != 0:
                        print(result.stderr)
                        fail("Failed to run perf2bolt.")
                    processed_fdata_files += 1
                    fdata_files.append(fdata_file_name)

            # If we have multiple fdata files we need to merge them together using merge-fdata
            if len(fdata_files) > 1:
                print("Merging fdata files with merge-fdata.")
                arguments = [repository_ctx.attr._merge_fdata_binary, "-o", bolt_fdata_filename] + fdata_files
                result = repository_ctx.execute(arguments)
                print(result.stdout)
                if result.return_code != 0:
                    print(result.stderr)
                    fail("Failed to run merge-fdata.")

                # clean up the pre-merged fdata files
                for file in fdata_files:
                    repository_ctx.delete(file)

    repository_ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "bolt_fdata",
    srcs = glob(["**/*.fdata"]),
)
""" + created_files,
    )

setup_bolt_data = repository_rule(
    implementation = _setup_bolt_data,
    environ = ["bolt_profile_url", "bolt_binary_url", "bolt_binary_name", "PATH"],
    attrs = {
        # There is a bug where the repo rule does not properly evaluate these labels so we have to list the full path to the binaries
        "_merge_fdata_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt/bin/merge-fdata", executable = True, cfg = "host"),
        "_perf2bolt_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt/bin/perf2bolt", executable = True, cfg = "host"),
        "_perf_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt/bin/perf", executable = True, cfg = "host"),
    },
)
