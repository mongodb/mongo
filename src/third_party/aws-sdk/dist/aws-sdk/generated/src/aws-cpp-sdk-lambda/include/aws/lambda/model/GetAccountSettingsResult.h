/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/AccountLimit.h>
#include <aws/lambda/model/AccountUsage.h>
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
  class GetAccountSettingsResult
  {
  public:
    AWS_LAMBDA_API GetAccountSettingsResult();
    AWS_LAMBDA_API GetAccountSettingsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetAccountSettingsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>Limits that are related to concurrency and code storage.</p>
     */
    inline const AccountLimit& GetAccountLimit() const{ return m_accountLimit; }
    inline void SetAccountLimit(const AccountLimit& value) { m_accountLimit = value; }
    inline void SetAccountLimit(AccountLimit&& value) { m_accountLimit = std::move(value); }
    inline GetAccountSettingsResult& WithAccountLimit(const AccountLimit& value) { SetAccountLimit(value); return *this;}
    inline GetAccountSettingsResult& WithAccountLimit(AccountLimit&& value) { SetAccountLimit(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of functions and amount of storage in use.</p>
     */
    inline const AccountUsage& GetAccountUsage() const{ return m_accountUsage; }
    inline void SetAccountUsage(const AccountUsage& value) { m_accountUsage = value; }
    inline void SetAccountUsage(AccountUsage&& value) { m_accountUsage = std::move(value); }
    inline GetAccountSettingsResult& WithAccountUsage(const AccountUsage& value) { SetAccountUsage(value); return *this;}
    inline GetAccountSettingsResult& WithAccountUsage(AccountUsage&& value) { SetAccountUsage(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetAccountSettingsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetAccountSettingsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetAccountSettingsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    AccountLimit m_accountLimit;

    AccountUsage m_accountUsage;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
