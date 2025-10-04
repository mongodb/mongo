/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/LayerVersionContentOutput.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/Runtime.h>
#include <aws/lambda/model/Architecture.h>
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
  class PublishLayerVersionResult
  {
  public:
    AWS_LAMBDA_API PublishLayerVersionResult();
    AWS_LAMBDA_API PublishLayerVersionResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API PublishLayerVersionResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>Details about the layer version.</p>
     */
    inline const LayerVersionContentOutput& GetContent() const{ return m_content; }
    inline void SetContent(const LayerVersionContentOutput& value) { m_content = value; }
    inline void SetContent(LayerVersionContentOutput&& value) { m_content = std::move(value); }
    inline PublishLayerVersionResult& WithContent(const LayerVersionContentOutput& value) { SetContent(value); return *this;}
    inline PublishLayerVersionResult& WithContent(LayerVersionContentOutput&& value) { SetContent(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the layer.</p>
     */
    inline const Aws::String& GetLayerArn() const{ return m_layerArn; }
    inline void SetLayerArn(const Aws::String& value) { m_layerArn = value; }
    inline void SetLayerArn(Aws::String&& value) { m_layerArn = std::move(value); }
    inline void SetLayerArn(const char* value) { m_layerArn.assign(value); }
    inline PublishLayerVersionResult& WithLayerArn(const Aws::String& value) { SetLayerArn(value); return *this;}
    inline PublishLayerVersionResult& WithLayerArn(Aws::String&& value) { SetLayerArn(std::move(value)); return *this;}
    inline PublishLayerVersionResult& WithLayerArn(const char* value) { SetLayerArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the layer version.</p>
     */
    inline const Aws::String& GetLayerVersionArn() const{ return m_layerVersionArn; }
    inline void SetLayerVersionArn(const Aws::String& value) { m_layerVersionArn = value; }
    inline void SetLayerVersionArn(Aws::String&& value) { m_layerVersionArn = std::move(value); }
    inline void SetLayerVersionArn(const char* value) { m_layerVersionArn.assign(value); }
    inline PublishLayerVersionResult& WithLayerVersionArn(const Aws::String& value) { SetLayerVersionArn(value); return *this;}
    inline PublishLayerVersionResult& WithLayerVersionArn(Aws::String&& value) { SetLayerVersionArn(std::move(value)); return *this;}
    inline PublishLayerVersionResult& WithLayerVersionArn(const char* value) { SetLayerVersionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The description of the version.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline void SetDescription(const Aws::String& value) { m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_description.assign(value); }
    inline PublishLayerVersionResult& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline PublishLayerVersionResult& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline PublishLayerVersionResult& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date that the layer version was created, in <a
     * href="https://www.w3.org/TR/NOTE-datetime">ISO-8601 format</a>
     * (YYYY-MM-DDThh:mm:ss.sTZD).</p>
     */
    inline const Aws::String& GetCreatedDate() const{ return m_createdDate; }
    inline void SetCreatedDate(const Aws::String& value) { m_createdDate = value; }
    inline void SetCreatedDate(Aws::String&& value) { m_createdDate = std::move(value); }
    inline void SetCreatedDate(const char* value) { m_createdDate.assign(value); }
    inline PublishLayerVersionResult& WithCreatedDate(const Aws::String& value) { SetCreatedDate(value); return *this;}
    inline PublishLayerVersionResult& WithCreatedDate(Aws::String&& value) { SetCreatedDate(std::move(value)); return *this;}
    inline PublishLayerVersionResult& WithCreatedDate(const char* value) { SetCreatedDate(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The version number.</p>
     */
    inline long long GetVersion() const{ return m_version; }
    inline void SetVersion(long long value) { m_version = value; }
    inline PublishLayerVersionResult& WithVersion(long long value) { SetVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The layer's compatible runtimes.</p> <p>The following list includes
     * deprecated runtimes. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtime-deprecation-levels">Runtime
     * use after deprecation</a>.</p> <p>For a list of all currently supported
     * runtimes, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtimes-supported">Supported
     * runtimes</a>.</p>
     */
    inline const Aws::Vector<Runtime>& GetCompatibleRuntimes() const{ return m_compatibleRuntimes; }
    inline void SetCompatibleRuntimes(const Aws::Vector<Runtime>& value) { m_compatibleRuntimes = value; }
    inline void SetCompatibleRuntimes(Aws::Vector<Runtime>&& value) { m_compatibleRuntimes = std::move(value); }
    inline PublishLayerVersionResult& WithCompatibleRuntimes(const Aws::Vector<Runtime>& value) { SetCompatibleRuntimes(value); return *this;}
    inline PublishLayerVersionResult& WithCompatibleRuntimes(Aws::Vector<Runtime>&& value) { SetCompatibleRuntimes(std::move(value)); return *this;}
    inline PublishLayerVersionResult& AddCompatibleRuntimes(const Runtime& value) { m_compatibleRuntimes.push_back(value); return *this; }
    inline PublishLayerVersionResult& AddCompatibleRuntimes(Runtime&& value) { m_compatibleRuntimes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The layer's software license.</p>
     */
    inline const Aws::String& GetLicenseInfo() const{ return m_licenseInfo; }
    inline void SetLicenseInfo(const Aws::String& value) { m_licenseInfo = value; }
    inline void SetLicenseInfo(Aws::String&& value) { m_licenseInfo = std::move(value); }
    inline void SetLicenseInfo(const char* value) { m_licenseInfo.assign(value); }
    inline PublishLayerVersionResult& WithLicenseInfo(const Aws::String& value) { SetLicenseInfo(value); return *this;}
    inline PublishLayerVersionResult& WithLicenseInfo(Aws::String&& value) { SetLicenseInfo(std::move(value)); return *this;}
    inline PublishLayerVersionResult& WithLicenseInfo(const char* value) { SetLicenseInfo(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of compatible <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/foundation-arch.html">instruction
     * set architectures</a>.</p>
     */
    inline const Aws::Vector<Architecture>& GetCompatibleArchitectures() const{ return m_compatibleArchitectures; }
    inline void SetCompatibleArchitectures(const Aws::Vector<Architecture>& value) { m_compatibleArchitectures = value; }
    inline void SetCompatibleArchitectures(Aws::Vector<Architecture>&& value) { m_compatibleArchitectures = std::move(value); }
    inline PublishLayerVersionResult& WithCompatibleArchitectures(const Aws::Vector<Architecture>& value) { SetCompatibleArchitectures(value); return *this;}
    inline PublishLayerVersionResult& WithCompatibleArchitectures(Aws::Vector<Architecture>&& value) { SetCompatibleArchitectures(std::move(value)); return *this;}
    inline PublishLayerVersionResult& AddCompatibleArchitectures(const Architecture& value) { m_compatibleArchitectures.push_back(value); return *this; }
    inline PublishLayerVersionResult& AddCompatibleArchitectures(Architecture&& value) { m_compatibleArchitectures.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PublishLayerVersionResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PublishLayerVersionResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PublishLayerVersionResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    LayerVersionContentOutput m_content;

    Aws::String m_layerArn;

    Aws::String m_layerVersionArn;

    Aws::String m_description;

    Aws::String m_createdDate;

    long long m_version;

    Aws::Vector<Runtime> m_compatibleRuntimes;

    Aws::String m_licenseInfo;

    Aws::Vector<Architecture> m_compatibleArchitectures;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
