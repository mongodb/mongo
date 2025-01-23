#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Types.h>

struct aws_endpoints_rule_engine;
struct aws_endpoints_request_context;
struct aws_endpoints_resolved_endpoint;

namespace Aws
{
    namespace Crt
    {
        namespace Endpoints
        {
            /*
             * Add parameter to the context.
             * Only string and boolean values are supported.
             * Adding parameter several times with the same name will overwrite
             * previous values.
             */
            class AWS_CRT_CPP_API RequestContext final
            {
              public:
                RequestContext(Allocator *allocator = ApiAllocator()) noexcept;
                ~RequestContext();

                /* TODO: move/copy semantics */
                RequestContext(const RequestContext &) = delete;
                RequestContext &operator=(const RequestContext &) = delete;
                RequestContext(RequestContext &&) = delete;
                RequestContext &operator=(RequestContext &&) = delete;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept { return m_requestContext != nullptr; }

                /*
                 * Add string parameter.
                 * True if added successfully and false if failed.
                 * Aws::Crt::LastError() can be used to retrieve failure error code.
                 */
                bool AddString(const ByteCursor &name, const ByteCursor &value);

                /*
                 * Add boolean parameter.
                 * True if added successfully and false if failed.
                 * Aws::Crt::LastError() can be used to retrieve failure error code.
                 */
                bool AddBoolean(const ByteCursor &name, bool value);

                /*
                 * Add string array parameter.
                 * True if added successfully and false if failed.
                 * Aws::Crt::LastError() can be used to retrieve failure error code.
                 */
                bool AddStringArray(const ByteCursor &name, const Vector<ByteCursor> &value);

                /// @private
                aws_endpoints_request_context *GetNativeHandle() const noexcept { return m_requestContext; }

              private:
                Allocator *m_allocator;
                aws_endpoints_request_context *m_requestContext;
            };

            /*
             * Outcome of Endpoint Resolution.
             * Outcome can be either endpoint (IsEndpoint) or error (IsError).
             * Endpoint outcome means that engine was able to resolve context to
             * an endpoint and outcome can have the following fields defined:
             * - Url (required) - resolved url
             * - Headers (optional) - additional headers to be included with request
             * - Properties (optional) - custom list of properties associated
             *   with request (json blob to be interpreted by the caller.)
             *
             * Error outcome means that context could not be resolved to an endpoint.
             * Outcome will have following fields:
             * - Error (required) - error message providing more info on why
             *   endpoint could not be resolved.
             */
            class AWS_CRT_CPP_API ResolutionOutcome final
            {
              public:
                ~ResolutionOutcome();

                /* TODO: move/copy semantics */
                ResolutionOutcome(const ResolutionOutcome &) = delete;
                ResolutionOutcome &operator=(const ResolutionOutcome &) = delete;
                ResolutionOutcome(ResolutionOutcome &&toMove) noexcept;
                ResolutionOutcome &operator=(ResolutionOutcome &&);

                bool IsEndpoint() const noexcept;
                bool IsError() const noexcept;

                /*
                 * Endpoint properties.
                 * Note: following fields are none if outcome is error.
                 * Headers and Properties are optional and could also be None.
                 */
                Optional<StringView> GetUrl() const;
                Optional<StringView> GetProperties() const;
                Optional<UnorderedMap<StringView, Vector<StringView>>> GetHeaders() const;

                /*
                 * Error properties.
                 * Note: following fields are none if outcome is error.
                 */
                Optional<StringView> GetError() const;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept { return m_resolvedEndpoint != nullptr; }

                /// @private For use by rule engine.
                ResolutionOutcome(aws_endpoints_resolved_endpoint *impl);

              private:
                aws_endpoints_resolved_endpoint *m_resolvedEndpoint;
            };

            /**
             * Endpoints Rule Engine.
             */
            class AWS_CRT_CPP_API RuleEngine final
            {
              public:
                RuleEngine(
                    const ByteCursor &rulesetCursor,
                    const ByteCursor &partitionsCursor,
                    Allocator *allocator = ApiAllocator()) noexcept;
                ~RuleEngine();

                RuleEngine(const RuleEngine &) = delete;
                RuleEngine &operator=(const RuleEngine &) = delete;
                RuleEngine(RuleEngine &&) = delete;
                RuleEngine &operator=(RuleEngine &&) = delete;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept { return m_ruleEngine != nullptr; }

                /*
                 * Resolves rules against the provided context.
                 * If successful return will have resolution outcome.
                 * If not, return will be none and Aws::Crt::LastError() can be
                 * used to retrieve CRT error code.
                 */
                Optional<ResolutionOutcome> Resolve(const RequestContext &context) const;

              private:
                aws_endpoints_rule_engine *m_ruleEngine;
            };
        } // namespace Endpoints
    } // namespace Crt
} // namespace Aws
