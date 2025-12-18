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
namespace os
{

/**
  Unique identifier for a particular build or compilation of the operating system.
 */
static constexpr const char *kOsBuildId = "os.build_id";

/**
  Human readable (not intended to be parsed) OS version information, like e.g. reported by @code ver
  @endcode or @code lsb_release -a @endcode commands.
 */
static constexpr const char *kOsDescription = "os.description";

/**
  Human readable operating system name.
 */
static constexpr const char *kOsName = "os.name";

/**
  The operating system type.
 */
static constexpr const char *kOsType = "os.type";

/**
  The version string of the operating system as defined in <a
  href="/docs/resource/README.md#version-attributes">Version Attributes</a>.
 */
static constexpr const char *kOsVersion = "os.version";

namespace OsTypeValues
{
/**
  Microsoft Windows
 */
static constexpr const char *kWindows = "windows";

/**
  Linux
 */
static constexpr const char *kLinux = "linux";

/**
  Apple Darwin
 */
static constexpr const char *kDarwin = "darwin";

/**
  FreeBSD
 */
static constexpr const char *kFreebsd = "freebsd";

/**
  NetBSD
 */
static constexpr const char *kNetbsd = "netbsd";

/**
  OpenBSD
 */
static constexpr const char *kOpenbsd = "openbsd";

/**
  DragonFly BSD
 */
static constexpr const char *kDragonflybsd = "dragonflybsd";

/**
  HP-UX (Hewlett Packard Unix)
 */
static constexpr const char *kHpux = "hpux";

/**
  AIX (Advanced Interactive eXecutive)
 */
static constexpr const char *kAix = "aix";

/**
  SunOS, Oracle Solaris
 */
static constexpr const char *kSolaris = "solaris";

/**
  Deprecated. Use @code zos @endcode instead.

  @deprecated
  {"note": "Replaced by @code zos @endcode.", "reason": "renamed", "renamed_to": "zos"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kZOs = "z_os";

/**
  IBM z/OS
 */
static constexpr const char *kZos = "zos";

}  // namespace OsTypeValues

}  // namespace os
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
