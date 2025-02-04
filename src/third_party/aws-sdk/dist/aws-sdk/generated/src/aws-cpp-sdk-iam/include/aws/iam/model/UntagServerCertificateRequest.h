/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class UntagServerCertificateRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UntagServerCertificateRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UntagServerCertificate"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the IAM server certificate from which you want to remove
     * tags.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetServerCertificateName() const{ return m_serverCertificateName; }
    inline bool ServerCertificateNameHasBeenSet() const { return m_serverCertificateNameHasBeenSet; }
    inline void SetServerCertificateName(const Aws::String& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = value; }
    inline void SetServerCertificateName(Aws::String&& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = std::move(value); }
    inline void SetServerCertificateName(const char* value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName.assign(value); }
    inline UntagServerCertificateRequest& WithServerCertificateName(const Aws::String& value) { SetServerCertificateName(value); return *this;}
    inline UntagServerCertificateRequest& WithServerCertificateName(Aws::String&& value) { SetServerCertificateName(std::move(value)); return *this;}
    inline UntagServerCertificateRequest& WithServerCertificateName(const char* value) { SetServerCertificateName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of key names as a simple array of strings. The tags with matching keys
     * are removed from the specified IAM server certificate.</p>
     */
    inline const Aws::Vector<Aws::String>& GetTagKeys() const{ return m_tagKeys; }
    inline bool TagKeysHasBeenSet() const { return m_tagKeysHasBeenSet; }
    inline void SetTagKeys(const Aws::Vector<Aws::String>& value) { m_tagKeysHasBeenSet = true; m_tagKeys = value; }
    inline void SetTagKeys(Aws::Vector<Aws::String>&& value) { m_tagKeysHasBeenSet = true; m_tagKeys = std::move(value); }
    inline UntagServerCertificateRequest& WithTagKeys(const Aws::Vector<Aws::String>& value) { SetTagKeys(value); return *this;}
    inline UntagServerCertificateRequest& WithTagKeys(Aws::Vector<Aws::String>&& value) { SetTagKeys(std::move(value)); return *this;}
    inline UntagServerCertificateRequest& AddTagKeys(const Aws::String& value) { m_tagKeysHasBeenSet = true; m_tagKeys.push_back(value); return *this; }
    inline UntagServerCertificateRequest& AddTagKeys(Aws::String&& value) { m_tagKeysHasBeenSet = true; m_tagKeys.push_back(std::move(value)); return *this; }
    inline UntagServerCertificateRequest& AddTagKeys(const char* value) { m_tagKeysHasBeenSet = true; m_tagKeys.push_back(value); return *this; }
    ///@}
  private:

    Aws::String m_serverCertificateName;
    bool m_serverCertificateNameHasBeenSet = false;

    Aws::Vector<Aws::String> m_tagKeys;
    bool m_tagKeysHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
