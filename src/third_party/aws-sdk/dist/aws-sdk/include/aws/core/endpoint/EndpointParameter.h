/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/client/AWSError.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

namespace Aws
{
    namespace Endpoint
    {
        class AWS_CORE_API EndpointParameter
        {
        public:
            enum class ParameterType
            {
                BOOLEAN,
                STRING,
                STRING_ARRAY
            };
            enum class ParameterOrigin
            {
                STATIC_CONTEXT,
                OPERATION_CONTEXT,
                CLIENT_CONTEXT,
                BUILT_IN,
                NOT_SET = -1
            };

            EndpointParameter(Aws::String name, bool initialValue, ParameterOrigin parameterOrigin = ParameterOrigin::NOT_SET)
                    : m_storedType(ParameterType::BOOLEAN),
                      m_parameterOrigin(parameterOrigin),
                      m_name(std::move(name)),
                      m_boolValue(initialValue)
            {}

            EndpointParameter(Aws::String name, Aws::String initialValue, ParameterOrigin parameterOrigin = ParameterOrigin::NOT_SET)
                    : m_storedType(ParameterType::STRING),
                      m_parameterOrigin(parameterOrigin),
                      m_name(std::move(name)),
                      m_stringValue(std::move(initialValue))
            {}

            EndpointParameter(Aws::String name, const char* initialValue, ParameterOrigin parameterOrigin = ParameterOrigin::NOT_SET)
                    : m_storedType(ParameterType::STRING),
                      m_parameterOrigin(parameterOrigin),
                      m_name(std::move(name)),
                      m_stringValue(initialValue)
            {}

            EndpointParameter(Aws::String name, const Aws::Vector<Aws::String>& initialValue, ParameterOrigin parameterOrigin = ParameterOrigin::NOT_SET)
                    : m_storedType(ParameterType::STRING_ARRAY),
                      m_parameterOrigin(parameterOrigin),
                      m_name(std::move(name)),
                      m_stringArrayValue(initialValue)
            {}

            EndpointParameter(ParameterType storedType, ParameterOrigin parameterOrigin, Aws::String name)
              : m_storedType(storedType),
                m_parameterOrigin(parameterOrigin),
                m_name(std::move(name))
            {}

            EndpointParameter(const EndpointParameter&) = default;
            EndpointParameter(EndpointParameter&&) = default;
            EndpointParameter& operator=(const EndpointParameter&) = default;
            EndpointParameter& operator=(EndpointParameter&&) = default;

            inline ParameterType GetStoredType() const
            {
                return m_storedType;
            }

            inline ParameterOrigin GetParameterOrigin() const
            {
                return m_parameterOrigin;
            }

            inline const Aws::String& GetName() const
            {
                return m_name;
            }

            enum class GetSetResult
            {
                SUCCESS,
                ERROR_WRONG_TYPE
            };

            inline GetSetResult GetBool(bool& ioValue) const
            {
                if(m_storedType != ParameterType::BOOLEAN)
                    return GetSetResult::ERROR_WRONG_TYPE;
                ioValue = m_boolValue;
                return GetSetResult::SUCCESS;
            }

            inline GetSetResult GetString(Aws::String& ioValue) const
            {
                // disabled RTTI...
                if(m_storedType != ParameterType::STRING)
                    return GetSetResult::ERROR_WRONG_TYPE;
                ioValue = m_stringValue;
                return GetSetResult::SUCCESS;
            }

            inline GetSetResult GetStringArray(Aws::Vector<Aws::String>& ioValue) const
            {
                if(m_storedType != ParameterType::STRING_ARRAY)
                    return GetSetResult::ERROR_WRONG_TYPE;
                ioValue = m_stringArrayValue;
                return GetSetResult::SUCCESS;
            }

            inline GetSetResult SetBool(bool iValue)
            {
                if(m_storedType != ParameterType::BOOLEAN)
                    return GetSetResult::ERROR_WRONG_TYPE;
                m_boolValue = iValue;
                return GetSetResult::SUCCESS;
            }

            inline GetSetResult SetString(Aws::String iValue)
            {
                if(m_storedType != ParameterType::STRING)
                    return GetSetResult::ERROR_WRONG_TYPE;
                m_stringValue = std::move(iValue);
                return GetSetResult::SUCCESS;
            }

            inline GetSetResult SetStringArray(const Aws::Vector<Aws::String>& iValue)
            {
                if(m_storedType != ParameterType::STRING_ARRAY)
                    return GetSetResult::ERROR_WRONG_TYPE;
                m_stringArrayValue = iValue;
                return GetSetResult::SUCCESS;
            }     

            inline GetSetResult SetStringArray(Aws::Vector<Aws::String>&& iValue)
            {
                if(m_storedType != ParameterType::STRING_ARRAY)
                    return GetSetResult::ERROR_WRONG_TYPE;
                m_stringArrayValue = std::move(iValue);
                return GetSetResult::SUCCESS;
            }        

            bool GetBoolValueNoCheck() const
            {
                return m_boolValue;
            }
            const Aws::String& GetStrValueNoCheck() const
            {
                return m_stringValue;
            }

            const Aws::Vector<Aws::String>& GetStrArrayValueNoCheck() const
            {
                return m_stringArrayValue;
            }

        protected:
            ParameterType m_storedType;
            ParameterOrigin m_parameterOrigin;
            Aws::String m_name;

            bool m_boolValue = false;
            Aws::String m_stringValue;
            Aws::Vector<Aws::String> m_stringArrayValue;
        };

        using EndpointParameters = Aws::Vector<Aws::Endpoint::EndpointParameter>;
    } // namespace Endpoint
} // namespace Aws
