/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketLocationResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

GetBucketLocationResult::GetBucketLocationResult():
    m_locationConstraint(BucketLocationConstraint::NOT_SET)
{
}

GetBucketLocationResult::GetBucketLocationResult(const AmazonWebServiceResult<XmlDocument>& result):
    m_locationConstraint(BucketLocationConstraint::NOT_SET)
{
    *this = result;
}

GetBucketLocationResult& GetBucketLocationResult::operator =(const AmazonWebServiceResult<XmlDocument>& result)
{
    const XmlDocument& xmlDocument = result.GetPayload();
    XmlNode resultNode = xmlDocument.GetRootElement();

    if(!resultNode.IsNull())
    {
        m_locationConstraint = BucketLocationConstraintMapper::GetBucketLocationConstraintForName(StringUtils::Trim(resultNode.GetText().c_str()).c_str());
    }

    return *this;
}
