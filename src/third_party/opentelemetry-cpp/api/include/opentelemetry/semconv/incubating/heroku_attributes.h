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
namespace heroku
{

/**
  Unique identifier for the application
 */
static constexpr const char *kHerokuAppId = "heroku.app.id";

/**
  Commit hash for the current release
 */
static constexpr const char *kHerokuReleaseCommit = "heroku.release.commit";

/**
  Time and date the release was created
 */
static constexpr const char *kHerokuReleaseCreationTimestamp = "heroku.release.creation_timestamp";

}  // namespace heroku
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
