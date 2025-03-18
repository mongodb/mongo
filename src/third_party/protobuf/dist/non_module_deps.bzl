load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _github_archive(repo, commit, **kwargs):
    repo_name = repo.split("/")[-1]
    http_archive(
        urls = [repo + "/archive/" + commit + ".zip"],
        strip_prefix = repo_name + "-" + commit,
        **kwargs
    )

def _non_module_deps_impl(ctx):
    _github_archive(
        name = "utf8_range",
        repo = "https://github.com/protocolbuffers/utf8_range",
        commit = "de0b4a8ff9b5d4c98108bdfe723291a33c52c54f",
        sha256 = "5da960e5e5d92394c809629a03af3c7709d2d3d0ca731dacb3a9fb4bf28f7702",
    )
    _github_archive(
        name = "rules_ruby",
        repo = "https://github.com/protocolbuffers/rules_ruby",
        commit = "b7f3e9756f3c45527be27bc38840d5a1ba690436",
        sha256 = "347927fd8de6132099fcdc58e8f7eab7bde4eb2fd424546b9cd4f1c6f8f8bad8",
    )

non_module_deps = module_extension(implementation = _non_module_deps_impl)

