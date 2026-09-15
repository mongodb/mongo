"""Backward-compatible entry point for the hermetic container integration.

The implementation lives in the hermetic_container package. This module keeps the
script invocation from tools/bazel and the historical import path
bazel.wrapper_hook.hermetic_container_integration working. The stdlib imports are
re-exported because tests patch attributes through this module (for example
hermetic_container_integration.urllib.request).
"""

import argparse  # noqa: F401
import contextlib  # noqa: F401
import dataclasses  # noqa: F401
import enum  # noqa: F401
import getpass  # noqa: F401
import hashlib  # noqa: F401
import json  # noqa: F401
import os  # noqa: F401
import pathlib  # noqa: F401
import platform  # noqa: F401
import re  # noqa: F401
import shlex  # noqa: F401
import shutil  # noqa: F401
import stat  # noqa: F401
import subprocess  # noqa: F401
import sys  # noqa: F401
import tempfile  # noqa: F401
import time  # noqa: F401
import types  # noqa: F401
import urllib.error  # noqa: F401
import urllib.request  # noqa: F401
import uuid  # noqa: F401
from collections.abc import Callable, Iterator, Mapping, Sequence  # noqa: F401

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from bazel.wrapper_hook.hermetic_container import (
    bazelrc as _bazelrc,
)
from bazel.wrapper_hook.hermetic_container import (
    cleanup as _cleanup,
)
from bazel.wrapper_hook.hermetic_container import (
    config as _config,
)
from bazel.wrapper_hook.hermetic_container import (
    constants as _constants,
)
from bazel.wrapper_hook.hermetic_container import (
    distro as _distro,
)
from bazel.wrapper_hook.hermetic_container import (
    docker as _docker,
)
from bazel.wrapper_hook.hermetic_container import (
    download as _download,
)
from bazel.wrapper_hook.hermetic_container import (
    engflow as _engflow,
)
from bazel.wrapper_hook.hermetic_container import (
    env as _env,
)
from bazel.wrapper_hook.hermetic_container import (
    fsutil as _fsutil,
)
from bazel.wrapper_hook.hermetic_container import (
    podman as _podman,
)
from bazel.wrapper_hook.hermetic_container import (
    runner as _runner,
)
from bazel.wrapper_hook.hermetic_container import (
    symlinks as _symlinks,
)
from bazel.wrapper_hook.hermetic_container import (
    volumes as _volumes,
)
from bazel.wrapper_hook.hermetic_container.bazelrc import (  # noqa: F401
    _bazel_command_line_options,
    _bazel_effective_options,
    _bazel_output_base,
    _bazelrc_paths,
    _bazelrc_startup_option_value,
    _bazelrc_tokens,
    _bool_build_setting_enabled,
    _config_requested,
    _credential_helper_requested,
    _effective_config_values,
    _remote_execution_disabled_by_args,
    _remote_execution_disabled_by_workspace_rc,
    _remote_execution_disabled_for_options,
    _remote_link_requested,
    _replace_bazel_startup_option,
    _repo_env_overrides_from_args,
    _resolve_bazelrc_path,
    _startup_boolean_option,
    _startup_option_value,
    _startup_option_values,
    _workspace_status_command_requested,
)
from bazel.wrapper_hook.hermetic_container.cleanup import (  # noqa: F401
    _clean_hermetic_container_outputs,
    _clean_host_outputs,
    _clean_requested_expunge,
    _expunge_hermetic_container_outputs,
)
from bazel.wrapper_hook.hermetic_container.config import (  # noqa: F401
    HermeticContainerConfig,
    IntegrationMode,
    _config_fingerprint,
    _hermetic_container_output_base,
    _run_file_matches_config,
    build_hermetic_container_config,
)
from bazel.wrapper_hook.hermetic_container.constants import (  # noqa: F401
    AL2023_DISTRO,
    CONTAINER_CGROUP_PATH,
    CONTAINER_CGROUP_RE,
    CONTAINER_MARKER_PATHS,
    DEFAULT_CONTAINER_CA_BUNDLE,
    DEFAULT_HERMETIC_CONTAINER_CONTAINER_ARCH,
    DEFAULT_HOST_CA_BUNDLE,
    DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
    HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV,
    HERMETIC_CONTAINER_DISABLED_VALUES,
    HERMETIC_CONTAINER_ENABLED_VALUES,
    HERMETIC_CONTAINER_GIT_LAYER_ENV,
    HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT_ENV,
    HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE_ENV,
    HERMETIC_CONTAINER_OUTPUT_BASE_PRESERVED_DIRS,
    HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION,
    HERMETIC_CONTAINER_REPOSITORY_PATH,
    HERMETIC_CONTAINER_ROOT_CONVENIENCE_SYMLINKS,
    HERMETIC_CONTAINER_SOURCE_ROOT,
    HERMETIC_CONTAINER_SYMLINK_PREFIX,
    HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND,
    KUBERNETES_SERVICE_HOST_ENV,
    LINUX_HOST_CONTAINER_ARCHES,
    LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS,
    MACOS_CROSS_DEFAULT_CONFIG_ENV,
    MACOS_CROSS_PASSTHROUGH_ENVS,
    MACOS_CROSS_PATH_ENVS,
    MONGO_TOOLCHAIN_VERSION_V5_FILE,
    RELEASE_LOCAL_SAFETY_SUFFIX,
    REMOTE_EXECUTION_CONTAINERS_FILE,
    REPO_ROOT,
    WINDOWS_CROSS_PASSTHROUGH_ENVS,
    WINDOWS_CROSS_PATH_ENVS,
    WINDOWS_CROSS_SDK_ROOT_ENVS,
    WINDOWS_TOOLCHAIN_PIN_ENVS,
)
from bazel.wrapper_hook.hermetic_container.cross import (
    common as _cross_common,
)
from bazel.wrapper_hook.hermetic_container.cross import (
    linux as _cross_linux,
)
from bazel.wrapper_hook.hermetic_container.cross import (
    macos as _cross_macos,
)
from bazel.wrapper_hook.hermetic_container.cross import (
    windows as _cross_windows,
)
from bazel.wrapper_hook.hermetic_container.cross.common import (  # noqa: F401
    CROSS_HOST_ACTION_CONFIG_FILENAME,
    RESMOKE_DEPS_PATH_MAP_ENV,
    RESMOKE_DEPS_PATH_SUFFIX,
    MacOSCrossHostRunPlan,
    MacOSCrossHostTestPlan,
    _add_test_env,
    _append_unique,
    _build_event_json_path,
    _cross_host_action_runtime_environment,
    _cross_host_run_plan,
    _exact_tag_query_regex,
    _expand_host_test_labels,
    _fallback_test_labels,
    _host_bazel_env,
    _host_binary_env,
    _host_test_base_env,
    _host_test_labels_from_build_event_json,
    _host_test_query_expression,
    _label_to_host_executable,
    _macos_cross_linux_python_options,
    _query_string,
    _read_resmoke_deps_path,
    _read_target_pattern_file,
    _resmoke_deps_path_file,
    _write_cross_host_action_config,
    _write_resmoke_deps_path_map,
)
from bazel.wrapper_hook.hermetic_container.cross.linux import (  # noqa: F401
    LINUX_CONTAINER_ACTION_WRAPPER_SCRIPT,
    LINUX_CONTAINER_ACTIONS_CONFIG_FILENAME,
    LINUX_CONTAINER_ACTIONS_ENV,
    LINUX_CONTAINER_ACTIONS_GENERATION_FILENAME,
    LINUX_CONTAINER_ACTIONS_LAYOUT_VERSION,
    LINUX_CONTAINER_ACTIONS_LOCK_FILENAME,
    LINUX_CONTAINER_TOOL_MNEMONICS,
    LINUX_CROSS_LOCAL_RELEASE_ENV_VARS,
    LINUX_CROSS_LOCAL_TOOL_MNEMONICS,
    LINUX_CROSS_MODULE_OVERRIDE_DIRNAME,
    LINUX_CROSS_MODULE_OVERRIDES,
    LINUX_CROSS_RBE_ARM_POOL,
    LINUX_CROSS_RBE_CONFIG_RE,
    LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV,
    LINUX_CROSS_RBE_DEFAULT_EXEC_ARCH,
    LINUX_CROSS_RBE_DEFAULT_EXEC_DISTRO,
    LINUX_CROSS_RBE_DEFAULT_TARGET_DISTRO,
    LINUX_CROSS_RBE_POOL_ENV,
    LINUX_CROSS_RBE_REMOTE_EXECUTOR,
    LINUX_CROSS_RBE_X86_POOL,
    LINUX_CROSS_REMOTE_COMPILE_MNEMONICS,
    LINUX_CROSS_TOOLCHAIN_ENV,
    LINUX_CROSS_TOOLCHAIN_REPOSITORY_PREFIXES,
    LINUX_DYNAMIC_CONTAINER_MNEMONICS,
    LINUX_DYNAMIC_LOCAL_LOAD_FACTOR,
    LINUX_DYNAMIC_SCHEDULING_ENV,
    LINUX_HOST_CONTAINER_COMMANDS,
    LINUX_LOCAL_CONTAINER_MNEMONICS,
    LINUX_LOCAL_JAVA_MNEMONICS,
    LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS,
    LINUX_LOCAL_TEST_MNEMONICS,
    NATIVE_TOOLCHAIN_CONFIG,
    WASI_SDK_EXEC_ARCH_ENV,
    LinuxCrossRBEConfig,
    _ensure_linux_action_container,
    _ensure_linux_container_image,
    _linux_container_actions_lock,
    _linux_cross_local_container_env,
    _linux_cross_local_process_env,
    _linux_cross_module_override_args,
    _linux_cross_module_tree_fingerprint,
    _linux_cross_rbe_config,
    _linux_cross_rbe_exec_properties,
    _linux_cross_rbe_execution_platform,
    _linux_cross_rbe_host_args,
    _linux_cross_rbe_local_host_args,
    _linux_cross_rbe_podman_repo_env,
    _linux_cross_rbe_podman_repo_env_args,
    _linux_cross_rbe_pool,
    _linux_cross_rbe_process_env,
    _linux_cross_target_runtime_test_env,
    _linux_cross_toolchain_selector,
    _linux_host_container_action_args,
    _linux_host_container_config,
    _linux_host_container_state_dir,
    _write_linux_container_actions_config,
    _write_linux_container_actions_config_unlocked,
)
from bazel.wrapper_hook.hermetic_container.cross.macos import (  # noqa: F401
    MACOS_CROSS_ACTION_WRAPPER_ENV,
    MACOS_CROSS_CONFIGS,
    MACOS_CROSS_DEFAULT_COMMANDS,
    MACOS_CROSS_DEFAULT_LOCAL_RESOURCE_VALUE,
    MACOS_CROSS_LOCAL_CPU_RESOURCES_ENV,
    MACOS_CROSS_LOCAL_TEST_JOBS_ENV,
    MACOS_CROSS_RBE_CONTAINER_IMAGE_ENV,
    MACOS_CROSS_RBE_POOL,
    MACOS_CROSS_RBE_POOL_ENV,
    MACOS_CROSS_REMOTE_EXECUTOR,
    MACOS_CROSS_SPLIT_TEST_RUNNER_ENV,
    MACOS_CROSS_TEST_RUNNER_ENV,
    _bazel_args_with_default_macos_cross_config,
    _default_macos_cross_config,
    _is_macos_cross_config_arg,
    _is_macos_cross_config_value,
    _macos_cross_action_container_env_options,
    _macos_cross_config_requested,
    _macos_cross_host_bazel_test_args,
    _macos_cross_host_bazel_test_candidate,
    _macos_cross_host_bazel_test_requested,
    _macos_cross_host_run_plan,
    _macos_cross_host_run_requested,
    _macos_cross_host_test_plan,
    _macos_cross_host_test_requested,
    _macos_cross_local_container_action_args,
    _macos_cross_local_container_action_candidate,
    _macos_cross_local_container_action_requested,
    _macos_cross_local_resource_options,
    _macos_cross_rbe_exec_properties,
    _macos_cross_remote_execution_disabled,
    _run_macos_cross_host_binary,
    _run_macos_cross_host_resmoke_tests,
    _run_macos_cross_host_tests,
    _should_default_macos_cross_config,
)
from bazel.wrapper_hook.hermetic_container.cross.windows import (  # noqa: F401
    WINDOWS_CROSS_ACTION_WRAPPER_ENV,
    WINDOWS_CROSS_CONFIG,
    WINDOWS_CROSS_DEFAULT_COMMANDS,
    WINDOWS_CROSS_DEFAULT_CONFIG_ENV,
    WINDOWS_CROSS_RBE_CONTAINER_IMAGE_ENV,
    WINDOWS_CROSS_RBE_POOL,
    WINDOWS_CROSS_RBE_POOL_ENV,
    WINDOWS_CROSS_REMOTE_EXECUTOR,
    WindowsCrossSysrootSpec,
    _bazel_args_with_default_windows_cross_config,
    _bazelrc_windows_repo_envs,
    _create_windows_cross_sysroot,
    _default_windows_sdk_root,
    _prepare_windows_cross_env,
    _remove_generated_sysroot,
    _required_windows_pin,
    _run_windows_cross_host_binary,
    _should_default_windows_cross_config,
    _should_default_windows_hermetic_container,
    _windows_cross_action_container_env_options,
    _windows_cross_config_requested,
    _windows_cross_default_llvm_path,
    _windows_cross_generated_sysroot_path,
    _windows_cross_host_run_plan,
    _windows_cross_host_run_requested,
    _windows_cross_host_wrapper_action_args,
    _windows_cross_host_wrapper_action_candidate,
    _windows_cross_host_wrapper_action_requested,
    _windows_cross_rbe_exec_properties,
    _windows_cross_remote_execution_disabled,
    _windows_cross_repo_env_values,
    _windows_cross_sysroot_archive_configured,
    _windows_cross_sysroot_is_current,
    _windows_cross_sysroot_manifest,
    _windows_cross_sysroot_spec,
)
from bazel.wrapper_hook.hermetic_container.distro import (  # noqa: F401
    _DISTRO_PATTERN_MAP,
    DockerImage,
    _is_digest_pinned_container_url,
    _linux_host_container_distro,
    _linux_host_container_supported,
    _parse_os_release,
    _read_bzl_mapping,
    _safe_name,
    _toolchain_key,
    detect_host_distro,
    has_mongo_toolchain,
    load_remote_execution_containers,
    normalize_arch,
    parse_docker_image,
    select_distro,
)
from bazel.wrapper_hook.hermetic_container.docker import (  # noqa: F401
    DOCKER_API_TOO_NEW_RE,
    WSL_DOCKER_API_VERSION_DEFAULT,
    WSL_DOCKER_API_VERSION_ENV,
    WSL_DOCKER_HOST_ENV,
    WSL_DOCKER_HOST_MODE,
    WSL_DRIVE_MOUNT_PREFIX_ENV,
    _docker_daemon_status,
    _print_docker_daemon_error,
    _select_linux_container_runtime,
    _use_wsl_docker,
)
from bazel.wrapper_hook.hermetic_container.download import (  # noqa: F401
    CONTAINER_BAZEL_ENV,
    LINUX_BAZEL_ENV,
    LINUX_BAZEL_SHA256_ENV,
    _bazel_version,
    _bazelisk_base_url,
    _default_container_bazel_arch,
    _download_file,
    _download_linux_bazel_checksum,
    _linux_bazel_cache_is_valid,
    _linux_bazel_checksum_file,
    _linux_bazel_filename,
    _linux_bazel_url,
    _parse_sha256,
    _read_linux_bazel_checksum,
    _resolve_container_bazel,
    _write_linux_bazel_checksum,
)
from bazel.wrapper_hook.hermetic_container.engflow import (  # noqa: F401
    ENGFLOW_AUTH_CLUSTER,
    ENGFLOW_AUTH_LINUX_RELEASES,
    ENGFLOW_AUTH_URL_PREFIX,
    HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER_ENV,
    _hermetic_container_engflow_auth_helper,
    _host_engflow_auth_helper,
    _host_engflow_env,
    _sync_engflow_file_token,
)
from bazel.wrapper_hook.hermetic_container.env import (  # noqa: F401
    BAZEL_COMMANDS,
    CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE,
    RUN_FLAG_PREFIXES,
    RUN_FLAGS_WITH_SEPARATE_VALUE,
    TEST_FLAG_PREFIXES,
    TEST_FLAGS_WITH_SEPARATE_VALUE,
    _append_bazel_command_options,
    _append_bazel_command_options_before_release_suffix,
    _append_bazel_command_options_last,
    _bazel_bool_flag_value,
    _bazel_command,
    _bazel_command_index,
    _bazel_run_target,
    _config_values,
    _consume_option_value,
    _env_is_false,
    _env_is_true,
    _info,
    _info_prefix,
    _is_cross_default_run_target,
    _is_macos_cross_default_run_target,
    _is_running_in_container,
    _parse_test_tag_filters,
    _platforms_requested,
    _supports_color,
    _warn_native_fallback,
    _warning,
)
from bazel.wrapper_hook.hermetic_container.fsutil import (  # noqa: F401
    CONTAINER_NETWORK_RETRY_ATTEMPTS,
    CONTAINER_NETWORK_RETRY_DELAY_SECONDS,
    _clean_directory_contents,
    _container_user,
    _current_user,
    _default_bazel_user_output_root,
    _ensure_linux_action_sandbox_base,
    _exclusive_file_lock,
    _hardlink_or_copy,
    _hermetic_container_bazel_user_output_root,
    _hermetic_container_home_dir,
    _hermetic_container_lock,
    _hermetic_container_state_dir,
    _host_home,
    _host_temp_shared_install_dir,
    _is_relative_to,
    _linux_action_sandbox_base,
    _linux_native_shared_install_dir,
    _linux_shared_install_dir,
    _macos_case_sensitive_output_dir,
    _macos_shared_install_dir,
    _parse_repo_env_assignment,
    _path_is_case_sensitive,
    _read_key_value_file,
    _remove_path,
    _run_container_network_command,
    _run_host_command,
    _sha256_file,
    _symlink_target,
)
from bazel.wrapper_hook.hermetic_container.podman import (  # noqa: F401
    PODMAN_AUTH_FILE_ENV,
    PODMAN_CONFIG_ENV,
    PODMAN_REPOSITORY_ENV_VARS,
    PODMAN_RUNTIME_DIR_PREFIX,
    PODMAN_STALE_RUNTIME_MARKER,
    PODMAN_STORAGE_DIR_PREFIX,
    PODMAN_TASK_ID_ENV,
    _compact_runtime_detail,
    _container_runtime_env,
    _ensure_owned_podman_directory,
    _is_podman_command,
    _podman_anonymous_runtime_env,
    _podman_auth_file,
    _podman_authentication_failure,
    _podman_command_detail,
    _podman_containers_config,
    _podman_has_running_containers,
    _podman_migration_force_is_allowed,
    _podman_migration_is_allowed,
    _podman_recovery_lock,
    _podman_runtime_dir,
    _podman_runtime_is_task_scoped,
    _podman_storage_config,
    _podman_task_root,
    _print_container_command_output,
    _recover_stale_podman_runtime,
    _reset_task_scoped_podman_runtime,
)
from bazel.wrapper_hook.hermetic_container.podman_common import (  # noqa: F401
    PODMAN_REQUIRED_ENV,
)
from bazel.wrapper_hook.hermetic_container.podman_common import (
    is_podman_docker_shim as _is_podman_docker_shim,  # noqa: F401
)
from bazel.wrapper_hook.hermetic_container.runner import (  # noqa: F401
    CONTAINERIZED_BES_KEYWORD,
    _bazel_args_with_container_bes_keyword,
    _bazel_args_with_hermetic_container_env,
    _bazel_args_with_native_install_strategy,
    _load_hermetic_container_module,
    _prepare_hermetic_container_process_env,
    _run_direct,
    _run_linux_native_fallback,
    _temporary_hermetic_container_engflow_bazelrc,
    main,
    run_hermetic_container,
    select_integration_mode,
    should_use_hermetic_container,
)
from bazel.wrapper_hook.hermetic_container.symlinks import (  # noqa: F401
    _bazel_args_with_hermetic_container_symlink_prefix,
    _hermetic_container_convenience_symlink_targets,
    _managed_convenience_symlink_targets,
    _publish_convenience_symlinks,
    _publish_hermetic_container_convenience_symlinks,
    _publish_linux_host_convenience_symlinks,
    _publish_linux_shared_install_symlink,
    _publish_macos_shared_install_symlink,
    _replace_symlink,
    _workspace_convenience_symlink_name,
    _write_hermetic_container_convenience_symlink_marker,
    restore_hermetic_container_convenience_symlinks_from_env,
)
from bazel.wrapper_hook.hermetic_container.volumes import (  # noqa: F401
    _collect_env_vars,
    _collect_volumes,
    _container_cert_env_values,
    _container_path,
    _hermetic_container_git_layer_dockerfile,
    _hermetic_container_git_layer_enabled,
    _hermetic_container_image_with_git_layer,
    _host_ca_bundle,
    _path_volume,
    _prepare_grpc_roots_dir,
)

_PATCHABLE_MODULES = (
    _constants,
    _env,
    _fsutil,
    _bazelrc,
    _distro,
    _download,
    _volumes,
    _engflow,
    _podman,
    _docker,
    _symlinks,
    _cleanup,
    _config,
    _runner,
    _cross_common,
    _cross_linux,
    _cross_macos,
    _cross_windows,
)


class _HermeticContainerIntegration(types.ModuleType):
    """Module type that propagates attribute writes to the implementation modules.

    Tests patch helpers through this facade (for example
    ``mock.patch.object(hermetic_container_integration, "_docker_daemon_status", ...)``).
    Because the implementation modules import those helpers into their own
    namespaces, a write here also updates every implementation module that binds
    the name, so the patch takes effect on all call paths.
    """

    def __setattr__(self, name: str, value: object) -> None:
        super().__setattr__(name, value)
        for module in _PATCHABLE_MODULES:
            if name in vars(module):
                setattr(module, name, value)


sys.modules[__name__].__class__ = _HermeticContainerIntegration


if __name__ == "__main__":
    sys.exit(main())
