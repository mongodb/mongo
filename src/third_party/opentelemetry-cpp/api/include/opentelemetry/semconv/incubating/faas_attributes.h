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
namespace faas
{

/**
  A boolean that is true if the serverless function is executed for the first time (aka cold-start).
 */
static constexpr const char *kFaasColdstart = "faas.coldstart";

/**
  A string containing the schedule period as <a
  href="https://docs.oracle.com/cd/E12058_01/doc/doc.1014/e12030/cron_expressions.htm">Cron
  Expression</a>.
 */
static constexpr const char *kFaasCron = "faas.cron";

/**
  The name of the source on which the triggering operation was performed. For example, in Cloud
  Storage or S3 corresponds to the bucket name, and in Cosmos DB to the database name.
 */
static constexpr const char *kFaasDocumentCollection = "faas.document.collection";

/**
  The document name/table subjected to the operation. For example, in Cloud Storage or S3 is the
  name of the file, and in Cosmos DB the table name.
 */
static constexpr const char *kFaasDocumentName = "faas.document.name";

/**
  Describes the type of the operation that was performed on the data.
 */
static constexpr const char *kFaasDocumentOperation = "faas.document.operation";

/**
  A string containing the time when the data was accessed in the <a
  href="https://www.iso.org/iso-8601-date-and-time-format.html">ISO 8601</a> format expressed in <a
  href="https://www.w3.org/TR/NOTE-datetime">UTC</a>.
 */
static constexpr const char *kFaasDocumentTime = "faas.document.time";

/**
  The execution environment ID as a string, that will be potentially reused for other invocations to
  the same function/function version. <ul> <li><strong>AWS Lambda:</strong> Use the (full) log
  stream name.</li>
  </ul>
 */
static constexpr const char *kFaasInstance = "faas.instance";

/**
  The invocation ID of the current function invocation.
 */
static constexpr const char *kFaasInvocationId = "faas.invocation_id";

/**
  The name of the invoked function.
  <p>
  SHOULD be equal to the @code faas.name @endcode resource attribute of the invoked function.
 */
static constexpr const char *kFaasInvokedName = "faas.invoked_name";

/**
  The cloud provider of the invoked function.
  <p>
  SHOULD be equal to the @code cloud.provider @endcode resource attribute of the invoked function.
 */
static constexpr const char *kFaasInvokedProvider = "faas.invoked_provider";

/**
  The cloud region of the invoked function.
  <p>
  SHOULD be equal to the @code cloud.region @endcode resource attribute of the invoked function.
 */
static constexpr const char *kFaasInvokedRegion = "faas.invoked_region";

/**
  The amount of memory available to the serverless function converted to Bytes.
  <p>
  It's recommended to set this attribute since e.g. too little memory can easily stop a Java AWS
  Lambda function from working correctly. On AWS Lambda, the environment variable @code
  AWS_LAMBDA_FUNCTION_MEMORY_SIZE @endcode provides this information (which must be multiplied by
  1,048,576).
 */
static constexpr const char *kFaasMaxMemory = "faas.max_memory";

/**
  The name of the single function that this runtime instance executes.
  <p>
  This is the name of the function as configured/deployed on the FaaS
  platform and is usually different from the name of the callback
  function (which may be stored in the
  <a href="/docs/general/attributes.md#source-code-attributes">@code code.namespace @endcode/@code
  code.function.name @endcode</a> span attributes). <p> For some cloud providers, the above
  definition is ambiguous. The following definition of function name MUST be used for this attribute
  (and consequently the span name) for the listed cloud providers/products:
  <ul>
    <li><strong>Azure:</strong>  The full name @code <FUNCAPP>/<FUNC> @endcode, i.e., function app
  name followed by a forward slash followed by the function name (this form can also be seen in the
  resource JSON for the function). This means that a span attribute MUST be used, as an Azure
  function app can host multiple functions that would usually share a TracerProvider (see also the
  @code cloud.resource_id @endcode attribute).</li>
  </ul>
 */
static constexpr const char *kFaasName = "faas.name";

/**
  A string containing the function invocation time in the <a
  href="https://www.iso.org/iso-8601-date-and-time-format.html">ISO 8601</a> format expressed in <a
  href="https://www.w3.org/TR/NOTE-datetime">UTC</a>.
 */
static constexpr const char *kFaasTime = "faas.time";

/**
  Type of the trigger which caused this function invocation.
 */
static constexpr const char *kFaasTrigger = "faas.trigger";

/**
  The immutable version of the function being executed.
  <p>
  Depending on the cloud provider and platform, use:
  <ul>
    <li><strong>AWS Lambda:</strong> The <a
  href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-versions.html">function
  version</a> (an integer represented as a decimal string).</li> <li><strong>Google Cloud Run
  (Services):</strong> The <a
  href="https://cloud.google.com/run/docs/managing/revisions">revision</a> (i.e., the function name
  plus the revision suffix).</li> <li><strong>Google Cloud Functions:</strong> The value of the <a
  href="https://cloud.google.com/run/docs/container-contract#services-env-vars">@code K_REVISION
  @endcode environment variable</a>.</li> <li><strong>Azure Functions:</strong> Not applicable. Do
  not set this attribute.</li>
  </ul>
 */
static constexpr const char *kFaasVersion = "faas.version";

namespace FaasDocumentOperationValues
{
/**
  When a new object is created.
 */
static constexpr const char *kInsert = "insert";

/**
  When an object is modified.
 */
static constexpr const char *kEdit = "edit";

/**
  When an object is deleted.
 */
static constexpr const char *kDelete = "delete";

}  // namespace FaasDocumentOperationValues

namespace FaasInvokedProviderValues
{
/**
  Alibaba Cloud
 */
static constexpr const char *kAlibabaCloud = "alibaba_cloud";

/**
  Amazon Web Services
 */
static constexpr const char *kAws = "aws";

/**
  Microsoft Azure
 */
static constexpr const char *kAzure = "azure";

/**
  Google Cloud Platform
 */
static constexpr const char *kGcp = "gcp";

/**
  Tencent Cloud
 */
static constexpr const char *kTencentCloud = "tencent_cloud";

}  // namespace FaasInvokedProviderValues

namespace FaasTriggerValues
{
/**
  A response to some data source operation such as a database or filesystem read/write
 */
static constexpr const char *kDatasource = "datasource";

/**
  To provide an answer to an inbound HTTP request
 */
static constexpr const char *kHttp = "http";

/**
  A function is set to be executed when messages are sent to a messaging system
 */
static constexpr const char *kPubsub = "pubsub";

/**
  A function is scheduled to be executed regularly
 */
static constexpr const char *kTimer = "timer";

/**
  If none of the others apply
 */
static constexpr const char *kOther = "other";

}  // namespace FaasTriggerValues

}  // namespace faas
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
