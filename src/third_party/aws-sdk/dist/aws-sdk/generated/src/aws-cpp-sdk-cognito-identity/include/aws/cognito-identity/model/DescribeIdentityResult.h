/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/DateTime.h>
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
namespace CognitoIdentity
{
namespace Model
{
  /**
   * <p>A description of the identity.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/IdentityDescription">AWS
   * API Reference</a></p>
   */
  class DescribeIdentityResult
  {
  public:
    AWS_COGNITOIDENTITY_API DescribeIdentityResult();
    AWS_COGNITOIDENTITY_API DescribeIdentityResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_COGNITOIDENTITY_API DescribeIdentityResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline void SetIdentityId(const Aws::String& value) { m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityId.assign(value); }
    inline DescribeIdentityResult& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline DescribeIdentityResult& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline DescribeIdentityResult& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The provider names.</p>
     */
    inline const Aws::Vector<Aws::String>& GetLogins() const{ return m_logins; }
    inline void SetLogins(const Aws::Vector<Aws::String>& value) { m_logins = value; }
    inline void SetLogins(Aws::Vector<Aws::String>&& value) { m_logins = std::move(value); }
    inline DescribeIdentityResult& WithLogins(const Aws::Vector<Aws::String>& value) { SetLogins(value); return *this;}
    inline DescribeIdentityResult& WithLogins(Aws::Vector<Aws::String>&& value) { SetLogins(std::move(value)); return *this;}
    inline DescribeIdentityResult& AddLogins(const Aws::String& value) { m_logins.push_back(value); return *this; }
    inline DescribeIdentityResult& AddLogins(Aws::String&& value) { m_logins.push_back(std::move(value)); return *this; }
    inline DescribeIdentityResult& AddLogins(const char* value) { m_logins.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Date on which the identity was created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreationDate() const{ return m_creationDate; }
    inline void SetCreationDate(const Aws::Utils::DateTime& value) { m_creationDate = value; }
    inline void SetCreationDate(Aws::Utils::DateTime&& value) { m_creationDate = std::move(value); }
    inline DescribeIdentityResult& WithCreationDate(const Aws::Utils::DateTime& value) { SetCreationDate(value); return *this;}
    inline DescribeIdentityResult& WithCreationDate(Aws::Utils::DateTime&& value) { SetCreationDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date on which the identity was last modified.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModifiedDate() const{ return m_lastModifiedDate; }
    inline void SetLastModifiedDate(const Aws::Utils::DateTime& value) { m_lastModifiedDate = value; }
    inline void SetLastModifiedDate(Aws::Utils::DateTime&& value) { m_lastModifiedDate = std::move(value); }
    inline DescribeIdentityResult& WithLastModifiedDate(const Aws::Utils::DateTime& value) { SetLastModifiedDate(value); return *this;}
    inline DescribeIdentityResult& WithLastModifiedDate(Aws::Utils::DateTime&& value) { SetLastModifiedDate(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DescribeIdentityResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DescribeIdentityResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DescribeIdentityResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_identityId;

    Aws::Vector<Aws::String> m_logins;

    Aws::Utils::DateTime m_creationDate;

    Aws::Utils::DateTime m_lastModifiedDate;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
