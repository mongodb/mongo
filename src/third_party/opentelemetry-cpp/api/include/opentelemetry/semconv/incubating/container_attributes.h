/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace container
{

/**
  The command used to run the container (i.e. the command name).
  <p>
  If using embedded credentials or sensitive data, it is recommended to remove them to prevent
  potential leakage.
 */
static constexpr const char *kContainerCommand = "container.command";

/**
  All the command arguments (including the command/executable itself) run by the container.
 */
static constexpr const char *kContainerCommandArgs = "container.command_args";

/**
  The full command run by the container as a single string representing the full command.
 */
static constexpr const char *kContainerCommandLine = "container.command_line";

/**
  Deprecated, use @code cpu.mode @endcode instead.

  @deprecated
  {"note": "Replaced by @code cpu.mode @endcode.", "reason": "renamed", "renamed_to": "cpu.mode"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kContainerCpuState = "container.cpu.state";

/**
  The name of the CSI (<a href="https://github.com/container-storage-interface/spec">Container
  Storage Interface</a>) plugin used by the volume. <p> This can sometimes be referred to as a
  "driver" in CSI implementations. This should represent the @code name @endcode field of the
  GetPluginInfo RPC.
 */
static constexpr const char *kContainerCsiPluginName = "container.csi.plugin.name";

/**
  The unique volume ID returned by the CSI (<a
  href="https://github.com/container-storage-interface/spec">Container Storage Interface</a>)
  plugin. <p> This can sometimes be referred to as a "volume handle" in CSI implementations. This
  should represent the @code Volume.volume_id @endcode field in CSI spec.
 */
static constexpr const char *kContainerCsiVolumeId = "container.csi.volume.id";

/**
  Container ID. Usually a UUID, as for example used to <a
  href="https://docs.docker.com/engine/containers/run/#container-identification">identify Docker
  containers</a>. The UUID might be abbreviated.
 */
static constexpr const char *kContainerId = "container.id";

/**
  Runtime specific image identifier. Usually a hash algorithm followed by a UUID.
  <p>
  Docker defines a sha256 of the image id; @code container.image.id @endcode corresponds to the
  @code Image @endcode field from the Docker container inspect <a
  href="https://docs.docker.com/reference/api/engine/version/v1.43/#tag/Container/operation/ContainerInspect">API</a>
  endpoint. K8s defines a link to the container registry repository with digest @code "imageID":
  "registry.azurecr.io
  /namespace/service/dockerfile@sha256:bdeabd40c3a8a492eaf9e8e44d0ebbb84bac7ee25ac0cf8a7159d25f62555625"
  @endcode. The ID is assigned by the container runtime and can vary in different environments.
  Consider using @code oci.manifest.digest @endcode if it is important to identify the same image in
  different environments/runtimes.
 */
static constexpr const char *kContainerImageId = "container.image.id";

/**
  Name of the image the container was built on.
 */
static constexpr const char *kContainerImageName = "container.image.name";

/**
  Repo digests of the container image as provided by the container runtime.
  <p>
  <a
  href="https://docs.docker.com/reference/api/engine/version/v1.43/#tag/Image/operation/ImageInspect">Docker</a>
  and <a
  href="https://github.com/kubernetes/cri-api/blob/c75ef5b473bbe2d0a4fc92f82235efd665ea8e9f/pkg/apis/runtime/v1/api.proto#L1237-L1238">CRI</a>
  report those under the @code RepoDigests @endcode field.
 */
static constexpr const char *kContainerImageRepoDigests = "container.image.repo_digests";

/**
  Container image tags. An example can be found in <a
  href="https://docs.docker.com/reference/api/engine/version/v1.43/#tag/Image/operation/ImageInspect">Docker
  Image Inspect</a>. Should be only the @code <tag> @endcode section of the full name for example
  from @code registry.example.com/my-org/my-image:<tag> @endcode.
 */
static constexpr const char *kContainerImageTags = "container.image.tags";

/**
  Container labels, @code <key> @endcode being the label name, the value being the label value.
  <p>
  For example, a docker container label @code app @endcode with value @code nginx @endcode SHOULD be
  recorded as the @code container.label.app @endcode attribute with value @code "nginx" @endcode.
 */
static constexpr const char *kContainerLabel = "container.label";

/**
  Deprecated, use @code container.label @endcode instead.

  @deprecated
  {"note": "Replaced by @code container.label @endcode.", "reason": "renamed", "renamed_to":
  "container.label"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kContainerLabels = "container.labels";

/**
  Container name used by container runtime.
 */
static constexpr const char *kContainerName = "container.name";

/**
  The container runtime managing this container.

  @deprecated
  {"note": "Replaced by @code container.runtime.name @endcode.", "reason": "renamed", "renamed_to":
  "container.runtime.name"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kContainerRuntime = "container.runtime";

/**
  A description about the runtime which could include, for example details about the CRI/API version
  being used or other customisations.
 */
static constexpr const char *kContainerRuntimeDescription = "container.runtime.description";

/**
  The container runtime managing this container.
 */
static constexpr const char *kContainerRuntimeName = "container.runtime.name";

/**
  The version of the runtime of this process, as returned by the runtime without modification.
 */
static constexpr const char *kContainerRuntimeVersion = "container.runtime.version";

namespace ContainerCpuStateValues
{
/**
  When tasks of the cgroup are in user mode (Linux). When all container processes are in user mode
  (Windows).
 */
static constexpr const char *kUser = "user";

/**
  When CPU is used by the system (host OS)
 */
static constexpr const char *kSystem = "system";

/**
  When tasks of the cgroup are in kernel mode (Linux). When all container processes are in kernel
  mode (Windows).
 */
static constexpr const char *kKernel = "kernel";

}  // namespace ContainerCpuStateValues

}  // namespace container
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
