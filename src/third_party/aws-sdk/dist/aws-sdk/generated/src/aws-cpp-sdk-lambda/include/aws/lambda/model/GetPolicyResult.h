/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{
  class GetPolicyResult
  {
  public:
    AWS_LAMBDA_API GetPolicyResult();
    AWS_LAMBDA_API GetPolicyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetPolicyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The resource-based policy.</p>
     */
    inline const Aws::String& GetPolicy() const{ return m_policy; }
    inline void SetPolicy(const Aws::String& value) { m_policy = value; }
    inline void SetPolicy(Aws::String&& value) { m_policy = std::move(value); }
    inline void SetPolicy(const char* value) { m_policy.assign(value); }
    inline GetPolicyResult& WithPolicy(const Aws::String& value) { SetPolicy(value); return *this;}
    inline GetPolicyResult& WithPolicy(Aws::String&& value) { SetPolicy(std::move(value)); return *this;}
    inline GetPolicyResult& WithPolicy(const char* value) { SetPolicy(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique identifier for the current revision of the policy.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionId.assign(value); }
    inline GetPolicyResult& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline GetPolicyResult& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline GetPolicyResult& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetPolicyResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetPolicyResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetPolicyResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_policy;

    Aws::String m_revisionId;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
