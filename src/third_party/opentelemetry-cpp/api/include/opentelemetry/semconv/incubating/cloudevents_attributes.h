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
namespace cloudevents
{

/**
  The <a href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#id">event_id</a>
  uniquely identifies the event.
 */
static constexpr const char *kCloudeventsEventId = "cloudevents.event_id";

/**
  The <a
  href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#source-1">source</a>
  identifies the context in which an event happened.
 */
static constexpr const char *kCloudeventsEventSource = "cloudevents.event_source";

/**
  The <a
  href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#specversion">version of
  the CloudEvents specification</a> which the event uses.
 */
static constexpr const char *kCloudeventsEventSpecVersion = "cloudevents.event_spec_version";

/**
  The <a
  href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#subject">subject</a> of
  the event in the context of the event producer (identified by source).
 */
static constexpr const char *kCloudeventsEventSubject = "cloudevents.event_subject";

/**
  The <a
  href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#type">event_type</a>
  contains a value describing the type of event related to the originating occurrence.
 */
static constexpr const char *kCloudeventsEventType = "cloudevents.event_type";

}  // namespace cloudevents
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
