/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/PolicyOwnerEntityType.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains details about the specified entity (user or role).</p> <p>This data
   * type is an element of the <a>EntityDetails</a> object.</p><p><h3>See Also:</h3> 
   * <a href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/EntityInfo">AWS
   * API Reference</a></p>
   */
  class EntityInfo
  {
  public:
    AWS_IAM_API EntityInfo();
    AWS_IAM_API EntityInfo(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API EntityInfo& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline EntityInfo& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline EntityInfo& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline EntityInfo& WithArn(const char* value) { SetArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the entity (user or role).</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline bool NameHasBeenSet() const { return m_nameHasBeenSet; }
    inline void SetName(const Aws::String& value) { m_nameHasBeenSet = true; m_name = value; }
    inline void SetName(Aws::String&& value) { m_nameHasBeenSet = true; m_name = std::move(value); }
    inline void SetName(const char* value) { m_nameHasBeenSet = true; m_name.assign(value); }
    inline EntityInfo& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline EntityInfo& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline EntityInfo& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of entity (user or role).</p>
     */
    inline const PolicyOwnerEntityType& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const PolicyOwnerEntityType& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(PolicyOwnerEntityType&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline EntityInfo& WithType(const PolicyOwnerEntityType& value) { SetType(value); return *this;}
    inline EntityInfo& WithType(PolicyOwnerEntityType&& value) { SetType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The identifier of the entity (user or role).</p>
     */
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline EntityInfo& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline EntityInfo& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline EntityInfo& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The path to the entity (user or role). For more information about paths, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline EntityInfo& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline EntityInfo& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline EntityInfo& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}
  private:

    Aws::String m_arn;
    bool m_arnHasBeenSet = false;

    Aws::String m_name;
    bool m_nameHasBeenSet = false;

    PolicyOwnerEntityType m_type;
    bool m_typeHasBeenSet = false;

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    Aws::String m_path;
    bool m_pathHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
