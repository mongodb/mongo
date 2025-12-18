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
namespace pprof
{

/**
  Provides an indication that multiple symbols map to this location's address, for example due to
  identical code folding by the linker. In that case the line information represents one of the
  multiple symbols. This field must be recomputed when the symbolization state of the profile
  changes.
 */
static constexpr const char *kPprofLocationIsFolded = "pprof.location.is_folded";

/**
  Indicates that there are filenames related to this mapping.
 */
static constexpr const char *kPprofMappingHasFilenames = "pprof.mapping.has_filenames";

/**
  Indicates that there are functions related to this mapping.
 */
static constexpr const char *kPprofMappingHasFunctions = "pprof.mapping.has_functions";

/**
  Indicates that there are inline frames related to this mapping.
 */
static constexpr const char *kPprofMappingHasInlineFrames = "pprof.mapping.has_inline_frames";

/**
  Indicates that there are line numbers related to this mapping.
 */
static constexpr const char *kPprofMappingHasLineNumbers = "pprof.mapping.has_line_numbers";

/**
  Free-form text associated with the profile. This field should not be used to store any
  machine-readable information, it is only for human-friendly content.
 */
static constexpr const char *kPprofProfileComment = "pprof.profile.comment";

}  // namespace pprof
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
