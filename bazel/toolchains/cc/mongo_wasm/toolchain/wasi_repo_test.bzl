load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(
    ":wasi_repo.bzl",
    "LINUX_TOOL_WRAPPER_TEMPLATE",
    "WASI_SDK_DIST",
    "cross_execution_arch",
    "linux_tool_wrapper_substitutions",
    "wasi_execution_arch",
)

def _linux_tool_wrapper_fixture_impl(ctx):
    wrappers = [ctx.outputs.clang, ctx.outputs.clangpp, ctx.outputs.ar]
    substitutions = linux_tool_wrapper_substitutions()
    for wrapper in wrappers:
        ctx.actions.expand_template(
            template = ctx.file._wrapper_template,
            output = wrapper,
            substitutions = substitutions[wrapper.basename],
            is_executable = True,
        )
    return [DefaultInfo(files = depset(wrappers))]

linux_tool_wrapper_fixture = rule(
    implementation = _linux_tool_wrapper_fixture_impl,
    attrs = {
        "_wrapper_template": attr.label(
            default = LINUX_TOOL_WRAPPER_TEMPLATE,
            allow_single_file = True,
        ),
    },
    outputs = {
        "clang": "%{name}/bin/mongo-wasm32-wasip2-clang",
        "clangpp": "%{name}/bin/mongo-wasm32-wasip2-clang++",
        "ar": "%{name}/bin/mongo-llvm-ar",
    },
)

def _fake_repository_context(environ, arch, name = "linux"):
    return struct(os = struct(environ = environ, arch = arch, name = name))

def _cross_execution_arch_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(
        env,
        "amd64",
        cross_execution_arch("rhel9_s390x_on_rhel9_x86_64"),
    )
    asserts.equals(
        env,
        "aarch64",
        cross_execution_arch("rhel9_ppc64le_on_rhel9_aarch64"),
    )
    asserts.equals(
        env,
        "amd64",
        cross_execution_arch("rhel8_s390x_on_rhel8_x86_64"),
    )
    asserts.equals(
        env,
        "aarch64",
        cross_execution_arch("rhel10_ppc64le_on_rhel10_aarch64"),
    )
    asserts.equals(env, None, cross_execution_arch("rhel9_s390x"))

    return unittest.end(env)

_cross_execution_arch_test = unittest.make(_cross_execution_arch_test_impl)

def _wasi_sdk_dist_test_impl(ctx):
    env = unittest.begin(ctx)

    # Maintaining existing wasm support requires an executable SDK for every
    # architecture the cross wrapper can select for remote actions (amd64 and
    # aarch64) and for native execution on the IBM target hosts (s390x and
    # ppc64le). A missing entry makes the repository rule fail hydration with
    # "Unsupported platform for wasi-sdk".
    for arch in ("amd64", "aarch64", "s390x", "ppc64le"):
        asserts.true(
            env,
            ("linux", arch) in WASI_SDK_DIST,
            "missing wasi-sdk build for linux/{}".format(arch),
        )

    # The IBM host archives must stay IBM-arch builds: the native persistent
    # container executes the SDK's llvm-ar directly through the uname dispatch
    # wrapper, and wasi_execution_arch falls back to the host architecture
    # outside cross-RBE builds.
    for arch in ("s390x", "ppc64le"):
        asserts.true(
            env,
            "mdb-build-public.s3.amazonaws.com" in WASI_SDK_DIST[("linux", arch)]["url"],
            "linux/{} must use the IBM-arch SDK archive".format(arch),
        )

    return unittest.end(env)

_wasi_sdk_dist_test = unittest.make(_wasi_sdk_dist_test_impl)

def _wasi_execution_arch_test_impl(ctx):
    env = unittest.begin(ctx)

    # A Windows client can hydrate an SDK that executes on Linux workers.
    asserts.equals(
        env,
        "amd64",
        wasi_execution_arch(_fake_repository_context({
            "MONGO_BAZEL_WASI_SDK_OS": "linux",
        }, "x86_64", name = "windows")),
    )

    # Canonical OS overrides use the SDK's architecture spelling, even when
    # the Bazel client runs on another OS.
    for host_os in ("linux", "mac os x"):
        asserts.equals(
            env,
            "x86_64",
            wasi_execution_arch(_fake_repository_context({
                "MONGO_BAZEL_WASI_SDK_OS": "macos",
            }, "x86_64", name = host_os)),
        )
        asserts.equals(
            env,
            "x86_64",
            wasi_execution_arch(_fake_repository_context({
                "MONGO_BAZEL_WASI_SDK_OS": "macos",
                "MONGO_WASI_SDK_EXEC_ARCH": "amd64",
            }, "aarch64", name = host_os)),
        )

    # The direct execution override is the source of truth and wins over a
    # stale selector during repository hydration.
    asserts.equals(
        env,
        "amd64",
        wasi_execution_arch(_fake_repository_context({
            "MONGO_LINUX_CROSS_TOOLCHAIN": "rhel9_s390x_on_rhel9_aarch64",
            "MONGO_WASI_SDK_EXEC_ARCH": "x86_64",
        }, "s390x")),
    )
    asserts.equals(
        env,
        "aarch64",
        wasi_execution_arch(_fake_repository_context({
            "MONGO_LINUX_CROSS_TOOLCHAIN": "rhel9_ppc64le_on_rhel9_aarch64",
        }, "ppc64le")),
    )
    asserts.equals(
        env,
        "amd64",
        wasi_execution_arch(_fake_repository_context({
            "MONGO_WASI_SDK_EXEC_ARCH": "x86_64",
        }, "s390x")),
    )
    asserts.equals(
        env,
        "aarch64",
        wasi_execution_arch(_fake_repository_context({
            "MONGO_WASI_SDK_EXEC_ARCH": "arm64",
        }, "s390x")),
    )
    asserts.equals(
        env,
        "s390x",
        wasi_execution_arch(_fake_repository_context({}, "s390x")),
    )
    asserts.equals(
        env,
        "ppc64le",
        wasi_execution_arch(_fake_repository_context({}, "ppc64le")),
    )

    # Linux archives use Bazel's amd64 spelling, while the macOS archive uses
    # x86_64.  The execution override must not leak a Linux-only spelling into
    # a native macOS hydration.
    asserts.equals(
        env,
        "x86_64",
        wasi_execution_arch(
            _fake_repository_context(
                {"MONGO_WASI_SDK_EXEC_ARCH": "x86_64"},
                "aarch64",
                name = "mac os x",
            ),
        ),
    )
    asserts.equals(
        env,
        "amd64",
        wasi_execution_arch(_fake_repository_context({}, "amd64", name = "windows")),
    )

    return unittest.end(env)

_wasi_execution_arch_test = unittest.make(_wasi_execution_arch_test_impl)

def wasi_repo_test_suite(name):
    unittest.suite(
        name,
        _cross_execution_arch_test,
        _wasi_sdk_dist_test,
        _wasi_execution_arch_test,
    )
