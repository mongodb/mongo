/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/string.h>
#include <aws/crt/Api.h>
#include <aws/crt/endpoints/RuleEngine.h>
#include <aws/sdkutils/endpoints_rule_engine.h>
#include <aws/sdkutils/partitions.h>

namespace Aws
{
    namespace Crt
    {
        namespace Endpoints
        {

            RequestContext::RequestContext(Allocator *allocator) noexcept : m_allocator(allocator)
            {
                m_requestContext = aws_endpoints_request_context_new(allocator);
            }

            RequestContext::~RequestContext()
            {
                m_requestContext = aws_endpoints_request_context_release(m_requestContext);
            }

            bool RequestContext::AddString(const ByteCursor &name, const ByteCursor &value)
            {
                return AWS_OP_SUCCESS !=
                       aws_endpoints_request_context_add_string(m_allocator, m_requestContext, name, value);
            }

            bool RequestContext::AddBoolean(const ByteCursor &name, bool value)
            {
                return AWS_OP_SUCCESS !=
                       aws_endpoints_request_context_add_boolean(m_allocator, m_requestContext, name, value);
            }

            bool RequestContext::AddStringArray(const ByteCursor &name, const Vector<ByteCursor> &value)
            {
                return AWS_OP_SUCCESS != aws_endpoints_request_context_add_string_array(
                                             m_allocator, m_requestContext, name, value.data(), value.size());
            }

            ResolutionOutcome::ResolutionOutcome(aws_endpoints_resolved_endpoint *impl) : m_resolvedEndpoint(impl) {}

            ResolutionOutcome::ResolutionOutcome(ResolutionOutcome &&toMove) noexcept
                : m_resolvedEndpoint(toMove.m_resolvedEndpoint)
            {
                toMove.m_resolvedEndpoint = nullptr;
            }

            ResolutionOutcome &ResolutionOutcome::operator=(ResolutionOutcome &&toMove)
            {
                if (&toMove != this)
                {
                    *this = ResolutionOutcome(std::move(toMove));
                }

                return *this;
            }

            ResolutionOutcome::~ResolutionOutcome()
            {
                aws_endpoints_resolved_endpoint_release(m_resolvedEndpoint);
            }

            bool ResolutionOutcome::IsEndpoint() const noexcept
            {
                return AWS_ENDPOINTS_RESOLVED_ENDPOINT == aws_endpoints_resolved_endpoint_get_type(m_resolvedEndpoint);
            }

            bool ResolutionOutcome::IsError() const noexcept
            {
                return AWS_ENDPOINTS_RESOLVED_ERROR == aws_endpoints_resolved_endpoint_get_type(m_resolvedEndpoint);
            }

            Optional<StringView> ResolutionOutcome::GetUrl() const
            {
                ByteCursor url;
                if (aws_endpoints_resolved_endpoint_get_url(m_resolvedEndpoint, &url))
                {
                    return Optional<StringView>();
                }

                return Optional<StringView>(ByteCursorToStringView(url));
            }

            inline StringView CrtStringToStringView(const aws_string *s)
            {
                ByteCursor key = aws_byte_cursor_from_string(s);
                return ByteCursorToStringView(key);
            }

            Optional<UnorderedMap<StringView, Vector<StringView>>> ResolutionOutcome::GetHeaders() const
            {
                const aws_hash_table *resolved_headers = nullptr;

                if (aws_endpoints_resolved_endpoint_get_headers(m_resolvedEndpoint, &resolved_headers))
                {
                    return Optional<UnorderedMap<StringView, Vector<StringView>>>();
                }

                UnorderedMap<StringView, Vector<StringView>> headers;
                for (struct aws_hash_iter iter = aws_hash_iter_begin(resolved_headers); !aws_hash_iter_done(&iter);
                     aws_hash_iter_next(&iter))
                {
                    ByteCursor key = aws_byte_cursor_from_string((const aws_string *)iter.element.key);
                    const aws_array_list *array = (const aws_array_list *)iter.element.value;
                    headers.emplace(std::make_pair(
                        ByteCursorToStringView(key),
                        ArrayListToVector<aws_string *, StringView>(array, CrtStringToStringView)));
                }

                return Optional<UnorderedMap<StringView, Vector<StringView>>>(headers);
            }

            Optional<StringView> ResolutionOutcome::GetProperties() const
            {
                ByteCursor properties;
                if (aws_endpoints_resolved_endpoint_get_properties(m_resolvedEndpoint, &properties))
                {
                    return Optional<StringView>();
                }

                return Optional<StringView>(ByteCursorToStringView(properties));
            }

            Optional<StringView> ResolutionOutcome::GetError() const
            {
                ByteCursor error;
                if (aws_endpoints_resolved_endpoint_get_error(m_resolvedEndpoint, &error))
                {
                    return Optional<StringView>();
                }

                return Optional<StringView>(ByteCursorToStringView(error));
            }

            RuleEngine::RuleEngine(
                const ByteCursor &rulesetCursor,
                const ByteCursor &partitionsCursor,
                Allocator *allocator) noexcept
                : m_ruleEngine(nullptr)
            {
                auto ruleset = aws_endpoints_ruleset_new_from_string(allocator, rulesetCursor);
                auto partitions = aws_partitions_config_new_from_string(allocator, partitionsCursor);
                if (ruleset != NULL && partitions != NULL)
                {
                    m_ruleEngine = aws_endpoints_rule_engine_new(allocator, ruleset, partitions);
                }

                if (ruleset != NULL)
                {
                    aws_endpoints_ruleset_release(ruleset);
                }

                if (partitions != NULL)
                {
                    aws_partitions_config_release(partitions);
                }
            }

            RuleEngine::~RuleEngine()
            {
                m_ruleEngine = aws_endpoints_rule_engine_release(m_ruleEngine);
            }

            Optional<ResolutionOutcome> RuleEngine::Resolve(const RequestContext &context) const
            {
                aws_endpoints_resolved_endpoint *resolved = NULL;
                if (aws_endpoints_rule_engine_resolve(m_ruleEngine, context.GetNativeHandle(), &resolved))
                {
                    return Optional<ResolutionOutcome>();
                }
                return Optional<ResolutionOutcome>(ResolutionOutcome(resolved));
            }
        } // namespace Endpoints
    } // namespace Crt
} // namespace Aws
