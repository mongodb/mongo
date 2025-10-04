/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/CodeSigningPolicy.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  /**
   * <p>Code signing configuration <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-codesigning.html#config-codesigning-policies">policies</a>
   * specify the validation failure action for signature mismatch or
   * expiry.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CodeSigningPolicies">AWS
   * API Reference</a></p>
   */
  class CodeSigningPolicies
  {
  public:
    AWS_LAMBDA_API CodeSigningPolicies();
    AWS_LAMBDA_API CodeSigningPolicies(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API CodeSigningPolicies& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>Code signing configuration policy for deployment validation failure. If you
     * set the policy to <code>Enforce</code>, Lambda blocks the deployment request if
     * signature validation checks fail. If you set the policy to <code>Warn</code>,
     * Lambda allows the deployment and creates a CloudWatch log. </p> <p>Default
     * value: <code>Warn</code> </p>
     */
    inline const CodeSigningPolicy& GetUntrustedArtifactOnDeployment() const{ return m_untrustedArtifactOnDeployment; }
    inline bool UntrustedArtifactOnDeploymentHasBeenSet() const { return m_untrustedArtifactOnDeploymentHasBeenSet; }
    inline void SetUntrustedArtifactOnDeployment(const CodeSigningPolicy& value) { m_untrustedArtifactOnDeploymentHasBeenSet = true; m_untrustedArtifactOnDeployment = value; }
    inline void SetUntrustedArtifactOnDeployment(CodeSigningPolicy&& value) { m_untrustedArtifactOnDeploymentHasBeenSet = true; m_untrustedArtifactOnDeployment = std::move(value); }
    inline CodeSigningPolicies& WithUntrustedArtifactOnDeployment(const CodeSigningPolicy& value) { SetUntrustedArtifactOnDeployment(value); return *this;}
    inline CodeSigningPolicies& WithUntrustedArtifactOnDeployment(CodeSigningPolicy&& value) { SetUntrustedArtifactOnDeployment(std::move(value)); return *this;}
    ///@}
  private:

    CodeSigningPolicy m_untrustedArtifactOnDeployment;
    bool m_untrustedArtifactOnDeploymentHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
