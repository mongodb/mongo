/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/Runtime.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Lambda
  {
    namespace Model
    {
      namespace RuntimeMapper
      {

        static const int nodejs_HASH = HashingUtils::HashString("nodejs");
        static const int nodejs4_3_HASH = HashingUtils::HashString("nodejs4.3");
        static const int nodejs6_10_HASH = HashingUtils::HashString("nodejs6.10");
        static const int nodejs8_10_HASH = HashingUtils::HashString("nodejs8.10");
        static const int nodejs10_x_HASH = HashingUtils::HashString("nodejs10.x");
        static const int nodejs12_x_HASH = HashingUtils::HashString("nodejs12.x");
        static const int nodejs14_x_HASH = HashingUtils::HashString("nodejs14.x");
        static const int nodejs16_x_HASH = HashingUtils::HashString("nodejs16.x");
        static const int java8_HASH = HashingUtils::HashString("java8");
        static const int java8_al2_HASH = HashingUtils::HashString("java8.al2");
        static const int java11_HASH = HashingUtils::HashString("java11");
        static const int python2_7_HASH = HashingUtils::HashString("python2.7");
        static const int python3_6_HASH = HashingUtils::HashString("python3.6");
        static const int python3_7_HASH = HashingUtils::HashString("python3.7");
        static const int python3_8_HASH = HashingUtils::HashString("python3.8");
        static const int python3_9_HASH = HashingUtils::HashString("python3.9");
        static const int dotnetcore1_0_HASH = HashingUtils::HashString("dotnetcore1.0");
        static const int dotnetcore2_0_HASH = HashingUtils::HashString("dotnetcore2.0");
        static const int dotnetcore2_1_HASH = HashingUtils::HashString("dotnetcore2.1");
        static const int dotnetcore3_1_HASH = HashingUtils::HashString("dotnetcore3.1");
        static const int dotnet6_HASH = HashingUtils::HashString("dotnet6");
        static const int dotnet8_HASH = HashingUtils::HashString("dotnet8");
        static const int nodejs4_3_edge_HASH = HashingUtils::HashString("nodejs4.3-edge");
        static const int go1_x_HASH = HashingUtils::HashString("go1.x");
        static const int ruby2_5_HASH = HashingUtils::HashString("ruby2.5");
        static const int ruby2_7_HASH = HashingUtils::HashString("ruby2.7");
        static const int provided_HASH = HashingUtils::HashString("provided");
        static const int provided_al2_HASH = HashingUtils::HashString("provided.al2");
        static const int nodejs18_x_HASH = HashingUtils::HashString("nodejs18.x");
        static const int python3_10_HASH = HashingUtils::HashString("python3.10");
        static const int java17_HASH = HashingUtils::HashString("java17");
        static const int ruby3_2_HASH = HashingUtils::HashString("ruby3.2");
        static const int ruby3_3_HASH = HashingUtils::HashString("ruby3.3");
        static const int python3_11_HASH = HashingUtils::HashString("python3.11");
        static const int nodejs20_x_HASH = HashingUtils::HashString("nodejs20.x");
        static const int provided_al2023_HASH = HashingUtils::HashString("provided.al2023");
        static const int python3_12_HASH = HashingUtils::HashString("python3.12");
        static const int java21_HASH = HashingUtils::HashString("java21");
        static const int python3_13_HASH = HashingUtils::HashString("python3.13");
        static const int nodejs22_x_HASH = HashingUtils::HashString("nodejs22.x");


        Runtime GetRuntimeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == nodejs_HASH)
          {
            return Runtime::nodejs;
          }
          else if (hashCode == nodejs4_3_HASH)
          {
            return Runtime::nodejs4_3;
          }
          else if (hashCode == nodejs6_10_HASH)
          {
            return Runtime::nodejs6_10;
          }
          else if (hashCode == nodejs8_10_HASH)
          {
            return Runtime::nodejs8_10;
          }
          else if (hashCode == nodejs10_x_HASH)
          {
            return Runtime::nodejs10_x;
          }
          else if (hashCode == nodejs12_x_HASH)
          {
            return Runtime::nodejs12_x;
          }
          else if (hashCode == nodejs14_x_HASH)
          {
            return Runtime::nodejs14_x;
          }
          else if (hashCode == nodejs16_x_HASH)
          {
            return Runtime::nodejs16_x;
          }
          else if (hashCode == java8_HASH)
          {
            return Runtime::java8;
          }
          else if (hashCode == java8_al2_HASH)
          {
            return Runtime::java8_al2;
          }
          else if (hashCode == java11_HASH)
          {
            return Runtime::java11;
          }
          else if (hashCode == python2_7_HASH)
          {
            return Runtime::python2_7;
          }
          else if (hashCode == python3_6_HASH)
          {
            return Runtime::python3_6;
          }
          else if (hashCode == python3_7_HASH)
          {
            return Runtime::python3_7;
          }
          else if (hashCode == python3_8_HASH)
          {
            return Runtime::python3_8;
          }
          else if (hashCode == python3_9_HASH)
          {
            return Runtime::python3_9;
          }
          else if (hashCode == dotnetcore1_0_HASH)
          {
            return Runtime::dotnetcore1_0;
          }
          else if (hashCode == dotnetcore2_0_HASH)
          {
            return Runtime::dotnetcore2_0;
          }
          else if (hashCode == dotnetcore2_1_HASH)
          {
            return Runtime::dotnetcore2_1;
          }
          else if (hashCode == dotnetcore3_1_HASH)
          {
            return Runtime::dotnetcore3_1;
          }
          else if (hashCode == dotnet6_HASH)
          {
            return Runtime::dotnet6;
          }
          else if (hashCode == dotnet8_HASH)
          {
            return Runtime::dotnet8;
          }
          else if (hashCode == nodejs4_3_edge_HASH)
          {
            return Runtime::nodejs4_3_edge;
          }
          else if (hashCode == go1_x_HASH)
          {
            return Runtime::go1_x;
          }
          else if (hashCode == ruby2_5_HASH)
          {
            return Runtime::ruby2_5;
          }
          else if (hashCode == ruby2_7_HASH)
          {
            return Runtime::ruby2_7;
          }
          else if (hashCode == provided_HASH)
          {
            return Runtime::provided;
          }
          else if (hashCode == provided_al2_HASH)
          {
            return Runtime::provided_al2;
          }
          else if (hashCode == nodejs18_x_HASH)
          {
            return Runtime::nodejs18_x;
          }
          else if (hashCode == python3_10_HASH)
          {
            return Runtime::python3_10;
          }
          else if (hashCode == java17_HASH)
          {
            return Runtime::java17;
          }
          else if (hashCode == ruby3_2_HASH)
          {
            return Runtime::ruby3_2;
          }
          else if (hashCode == ruby3_3_HASH)
          {
            return Runtime::ruby3_3;
          }
          else if (hashCode == python3_11_HASH)
          {
            return Runtime::python3_11;
          }
          else if (hashCode == nodejs20_x_HASH)
          {
            return Runtime::nodejs20_x;
          }
          else if (hashCode == provided_al2023_HASH)
          {
            return Runtime::provided_al2023;
          }
          else if (hashCode == python3_12_HASH)
          {
            return Runtime::python3_12;
          }
          else if (hashCode == java21_HASH)
          {
            return Runtime::java21;
          }
          else if (hashCode == python3_13_HASH)
          {
            return Runtime::python3_13;
          }
          else if (hashCode == nodejs22_x_HASH)
          {
            return Runtime::nodejs22_x;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<Runtime>(hashCode);
          }

          return Runtime::NOT_SET;
        }

        Aws::String GetNameForRuntime(Runtime enumValue)
        {
          switch(enumValue)
          {
          case Runtime::NOT_SET:
            return {};
          case Runtime::nodejs:
            return "nodejs";
          case Runtime::nodejs4_3:
            return "nodejs4.3";
          case Runtime::nodejs6_10:
            return "nodejs6.10";
          case Runtime::nodejs8_10:
            return "nodejs8.10";
          case Runtime::nodejs10_x:
            return "nodejs10.x";
          case Runtime::nodejs12_x:
            return "nodejs12.x";
          case Runtime::nodejs14_x:
            return "nodejs14.x";
          case Runtime::nodejs16_x:
            return "nodejs16.x";
          case Runtime::java8:
            return "java8";
          case Runtime::java8_al2:
            return "java8.al2";
          case Runtime::java11:
            return "java11";
          case Runtime::python2_7:
            return "python2.7";
          case Runtime::python3_6:
            return "python3.6";
          case Runtime::python3_7:
            return "python3.7";
          case Runtime::python3_8:
            return "python3.8";
          case Runtime::python3_9:
            return "python3.9";
          case Runtime::dotnetcore1_0:
            return "dotnetcore1.0";
          case Runtime::dotnetcore2_0:
            return "dotnetcore2.0";
          case Runtime::dotnetcore2_1:
            return "dotnetcore2.1";
          case Runtime::dotnetcore3_1:
            return "dotnetcore3.1";
          case Runtime::dotnet6:
            return "dotnet6";
          case Runtime::dotnet8:
            return "dotnet8";
          case Runtime::nodejs4_3_edge:
            return "nodejs4.3-edge";
          case Runtime::go1_x:
            return "go1.x";
          case Runtime::ruby2_5:
            return "ruby2.5";
          case Runtime::ruby2_7:
            return "ruby2.7";
          case Runtime::provided:
            return "provided";
          case Runtime::provided_al2:
            return "provided.al2";
          case Runtime::nodejs18_x:
            return "nodejs18.x";
          case Runtime::python3_10:
            return "python3.10";
          case Runtime::java17:
            return "java17";
          case Runtime::ruby3_2:
            return "ruby3.2";
          case Runtime::ruby3_3:
            return "ruby3.3";
          case Runtime::python3_11:
            return "python3.11";
          case Runtime::nodejs20_x:
            return "nodejs20.x";
          case Runtime::provided_al2023:
            return "provided.al2023";
          case Runtime::python3_12:
            return "python3.12";
          case Runtime::java21:
            return "java21";
          case Runtime::python3_13:
            return "python3.13";
          case Runtime::nodejs22_x:
            return "nodejs22.x";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace RuntimeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
