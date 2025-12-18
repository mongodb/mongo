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
namespace host
{

/**
  The CPU architecture the host system is running on.
 */
static constexpr const char *kHostArch = "host.arch";

/**
  The amount of level 2 memory cache available to the processor (in Bytes).
 */
static constexpr const char *kHostCpuCacheL2Size = "host.cpu.cache.l2.size";

/**
  Family or generation of the CPU.
 */
static constexpr const char *kHostCpuFamily = "host.cpu.family";

/**
  Model identifier. It provides more granular information about the CPU, distinguishing it from
  other CPUs within the same family.
 */
static constexpr const char *kHostCpuModelId = "host.cpu.model.id";

/**
  Model designation of the processor.
 */
static constexpr const char *kHostCpuModelName = "host.cpu.model.name";

/**
  Stepping or core revisions.
 */
static constexpr const char *kHostCpuStepping = "host.cpu.stepping";

/**
  Processor manufacturer identifier. A maximum 12-character string.
  <p>
  <a href="https://wiki.osdev.org/CPUID">CPUID</a> command returns the vendor ID string in EBX, EDX
  and ECX registers. Writing these to memory in this order results in a 12-character string.
 */
static constexpr const char *kHostCpuVendorId = "host.cpu.vendor.id";

/**
  Unique host ID. For Cloud, this must be the instance_id assigned by the cloud provider. For
  non-containerized systems, this should be the @code machine-id @endcode. See the table below for
  the sources to use to determine the @code machine-id @endcode based on operating system.
 */
static constexpr const char *kHostId = "host.id";

/**
  VM image ID or host OS image ID. For Cloud, this value is from the provider.
 */
static constexpr const char *kHostImageId = "host.image.id";

/**
  Name of the VM image or OS install the host was instantiated from.
 */
static constexpr const char *kHostImageName = "host.image.name";

/**
  The version string of the VM image or host OS as defined in <a
  href="/docs/resource/README.md#version-attributes">Version Attributes</a>.
 */
static constexpr const char *kHostImageVersion = "host.image.version";

/**
  Available IP addresses of the host, excluding loopback interfaces.
  <p>
  IPv4 Addresses MUST be specified in dotted-quad notation. IPv6 addresses MUST be specified in the
  <a href="https://www.rfc-editor.org/rfc/rfc5952.html">RFC 5952</a> format.
 */
static constexpr const char *kHostIp = "host.ip";

/**
  Available MAC addresses of the host, excluding loopback interfaces.
  <p>
  MAC Addresses MUST be represented in <a
  href="https://standards.ieee.org/wp-content/uploads/import/documents/tutorials/eui.pdf">IEEE RA
  hexadecimal form</a>: as hyphen-separated octets in uppercase hexadecimal form from most to least
  significant.
 */
static constexpr const char *kHostMac = "host.mac";

/**
  Name of the host. On Unix systems, it may contain what the hostname command returns, or the fully
  qualified hostname, or another name specified by the user.
 */
static constexpr const char *kHostName = "host.name";

/**
  Type of host. For Cloud, this must be the machine type.
 */
static constexpr const char *kHostType = "host.type";

namespace HostArchValues
{
/**
  AMD64
 */
static constexpr const char *kAmd64 = "amd64";

/**
  ARM32
 */
static constexpr const char *kArm32 = "arm32";

/**
  ARM64
 */
static constexpr const char *kArm64 = "arm64";

/**
  Itanium
 */
static constexpr const char *kIa64 = "ia64";

/**
  32-bit PowerPC
 */
static constexpr const char *kPpc32 = "ppc32";

/**
  64-bit PowerPC
 */
static constexpr const char *kPpc64 = "ppc64";

/**
  IBM z/Architecture
 */
static constexpr const char *kS390x = "s390x";

/**
  32-bit x86
 */
static constexpr const char *kX86 = "x86";

}  // namespace HostArchValues

}  // namespace host
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
