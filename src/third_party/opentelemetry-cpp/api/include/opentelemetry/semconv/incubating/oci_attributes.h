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
namespace oci
{

/**
  The digest of the OCI image manifest. For container images specifically is the digest by which the
  container image is known. <p> Follows <a
  href="https://github.com/opencontainers/image-spec/blob/main/manifest.md">OCI Image Manifest
  Specification</a>, and specifically the <a
  href="https://github.com/opencontainers/image-spec/blob/main/descriptor.md#digests">Digest
  property</a>. An example can be found in <a
  href="https://github.com/opencontainers/image-spec/blob/main/manifest.md#example-image-manifest">Example
  Image Manifest</a>.
 */
static constexpr const char *kOciManifestDigest = "oci.manifest.digest";

}  // namespace oci
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
