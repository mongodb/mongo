/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class CreatePolicyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreatePolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreatePolicy"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The friendly name of the policy.</p> <p>IAM user, group, role, and policy
     * names must be unique within the account. Names are not distinguished by case.
     * For example, you cannot create resources named both "MyResource" and
     * "myresource".</p>
     */
    inline const Aws::String& GetPolicyName() const{ return m_policyName; }
    inline bool PolicyNameHasBeenSet() const { return m_policyNameHasBeenSet; }
    inline void SetPolicyName(const Aws::String& value) { m_policyNameHasBeenSet = true; m_policyName = value; }
    inline void SetPolicyName(Aws::String&& value) { m_policyNameHasBeenSet = true; m_policyName = std::move(value); }
    inline void SetPolicyName(const char* value) { m_policyNameHasBeenSet = true; m_policyName.assign(value); }
    inline CreatePolicyRequest& WithPolicyName(const Aws::String& value) { SetPolicyName(value); return *this;}
    inline CreatePolicyRequest& WithPolicyName(Aws::String&& value) { SetPolicyName(std::move(value)); return *this;}
    inline CreatePolicyRequest& WithPolicyName(const char* value) { SetPolicyName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The path for the policy.</p> <p>For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p> <p>This parameter is optional.
     * If it is not included, it defaults to a slash (/).</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of either a forward slash (/) by itself or a
     * string that must begin and end with forward slashes. In addition, it can contain
     * any ASCII character from the ! (<code>\u0021</code>) through the DEL character
     * (<code>\u007F</code>), including most punctuation characters, digits, and upper
     * and lowercased letters.</p>  <p>You cannot use an asterisk (*) in the path
     * name.</p> 
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline CreatePolicyRequest& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline CreatePolicyRequest& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline CreatePolicyRequest& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The JSON policy document that you want to use as the content for the new
     * policy.</p> <p>You must provide policies in JSON format in IAM. However, for
     * CloudFormation templates formatted in YAML, you can provide the policy in JSON
     * or YAML format. CloudFormation always converts a YAML policy to JSON format
     * before submitting it to IAM.</p> <p>The maximum length of the policy document
     * that you can pass in this operation, including whitespace, is listed below. To
     * view the maximum character counts of a managed policy with no whitespaces, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_iam-quotas.html#reference_iam-quotas-entity-length">IAM
     * and STS character quotas</a>.</p> <p>To learn more about JSON policy grammar,
     * see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_grammar.html">Grammar
     * of the IAM JSON policy language</a> in the <i>IAM User Guide</i>. </p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::String& GetPolicyDocument() const{ return m_policyDocument; }
    inline bool PolicyDocumentHasBeenSet() const { return m_policyDocumentHasBeenSet; }
    inline void SetPolicyDocument(const Aws::String& value) { m_policyDocumentHasBeenSet = true; m_policyDocument = value; }
    inline void SetPolicyDocument(Aws::String&& value) { m_policyDocumentHasBeenSet = true; m_policyDocument = std::move(value); }
    inline void SetPolicyDocument(const char* value) { m_policyDocumentHasBeenSet = true; m_policyDocument.assign(value); }
    inline CreatePolicyRequest& WithPolicyDocument(const Aws::String& value) { SetPolicyDocument(value); return *this;}
    inline CreatePolicyRequest& WithPolicyDocument(Aws::String&& value) { SetPolicyDocument(std::move(value)); return *this;}
    inline CreatePolicyRequest& WithPolicyDocument(const char* value) { SetPolicyDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A friendly description of the policy.</p> <p>Typically used to store
     * information about the permissions defined in the policy. For example, "Grants
     * access to production DynamoDB tables."</p> <p>The policy description is
     * immutable. After a value is assigned, it cannot be changed.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline CreatePolicyRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CreatePolicyRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CreatePolicyRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that you want to attach to the new IAM customer managed
     * policy. Each tag consists of a key name and an associated value. For more
     * information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>  <p>If any one of the tags
     * is invalid or if you exceed the allowed maximum number of tags, then the entire
     * request fails and the resource is not created.</p> 
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline CreatePolicyRequest& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline CreatePolicyRequest& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline CreatePolicyRequest& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline CreatePolicyRequest& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_policyName;
    bool m_policyNameHasBeenSet = false;

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_policyDocument;
    bool m_policyDocumentHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
