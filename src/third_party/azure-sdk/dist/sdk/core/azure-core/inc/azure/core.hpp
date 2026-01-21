// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @brief Includes all public headers from Azure Core SDK library.
 * @note The transport adapter headers are not included.
 */

#pragma once

// azure/core
#include "azure/core/azure_assert.hpp"
#include "azure/core/base64.hpp"
#include "azure/core/case_insensitive_containers.hpp"
#include "azure/core/context.hpp"
#include "azure/core/datetime.hpp"
#include "azure/core/dll_import_export.hpp"
#include "azure/core/etag.hpp"
#include "azure/core/exception.hpp"
#include "azure/core/match_conditions.hpp"
#include "azure/core/modified_conditions.hpp"
#include "azure/core/nullable.hpp"
#include "azure/core/operation.hpp"
#include "azure/core/operation_status.hpp"
#include "azure/core/paged_response.hpp"
#include "azure/core/platform.hpp"
#include "azure/core/response.hpp"
#include "azure/core/rtti.hpp"
#include "azure/core/url.hpp"
#include "azure/core/uuid.hpp"

// azure/core/credentials
#include "azure/core/credentials/credentials.hpp"
#include "azure/core/credentials/token_credential_options.hpp"

// azure/core/cryptography
#include "azure/core/cryptography/hash.hpp"

// azure/core/diagnostics
#include "azure/core/diagnostics/logger.hpp"

// azure/core/http
#include "azure/core/http/http.hpp"
#include "azure/core/http/http_status_code.hpp"
#include "azure/core/http/raw_response.hpp"
#include "azure/core/http/transport.hpp"

// azure/core/http/policies
#include "azure/core/http/policies/policy.hpp"

// azure/core/io
#include "azure/core/io/body_stream.hpp"

// azure/core/tracing
#include "azure/core/tracing/tracing.hpp"
