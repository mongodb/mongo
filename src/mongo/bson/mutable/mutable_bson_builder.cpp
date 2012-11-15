/* Copyright 2010 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License->
 * You may obtain a copy of the License at
 *
 * http://www.apache->org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License->
 */

#include "mongo/bson/mutable/mutable_bson_builder.h"

#include <sstream>

#include "mongo/base/status.h"
#include "mongo/base/error_codes.h"

#define __TRACE__  __FILE__ << ":" << __FUNCTION__ << " [" << __LINE__ << "]"

namespace mongo {
namespace mutablebson {

    //
    // ElementBuilder
    //

    Status ElementBuilder::parse(Element* dst, const BSONObj& src) {
        Status result(Status::OK());
        Document& doc = *dst->getDocument();

        BSONObjIterator it(src);
        for (; it.more(); ++it) {
            BSONElement bsonElem = *it;

            // TODO: We should have a way to get the fieldName as a StringData.
            const char* fieldName = bsonElem.fieldName();

            switch (bsonElem.type()) {
            case MinKey: {
                result = dst->appendMinKey(fieldName);
                break;
            }
            case NumberDouble: {
                result = dst->appendDouble(fieldName, bsonElem.Double());
                break;
            }
            case String: {
                result = dst->appendString(fieldName, bsonElem.String().c_str());
                break;
            }
            case Object: {
                Element e0 = doc.makeObjElement(fieldName);
                result = ElementBuilder::parse(&e0, bsonElem.Obj());
                if (result.isOK())
                    result = dst->appendElement(fieldName, e0);
                break;
            }
            case Array: {
                Element e0 = doc.makeArrayElement(fieldName);
                result = ElementBuilder::parse(&e0, bsonElem.Obj());
                if (result.isOK())
                    result = dst->appendElement(fieldName, e0);
                break;
            }
            case BinData: {  // a stub
                break;
            }
            case Undefined: { // a stub
                break;
            }
            case jstOID: {
                result = dst->appendOID(fieldName, bsonElem.OID());
                break;
            }
            case Bool: {
                result = dst->appendBool(fieldName, bsonElem.Bool());
                break;
            }
            case Date: {
                result = dst->appendDate(fieldName, bsonElem.Date().millis);
                break;
            }
            case jstNULL: {
                result = dst->appendNull(fieldName);
                break;
            }
            case RegEx: {
                result = dst->appendRegex(fieldName, bsonElem.regex(), bsonElem.regexFlags());
                break;
            }
            case DBRef: {
                result = dst->appendDBRef(fieldName, bsonElem.dbrefNS(), bsonElem.dbrefOID());
                break;
            }
            /** Note: The following three cases seem fishy. They need better understanding. */
            case Code: {
                result = dst->appendCode(fieldName, bsonElem.str().c_str());
                break;
            }
            case Symbol: {
                result = dst->appendSymbol(fieldName, bsonElem.str().c_str());
                break;
            }
            case CodeWScope: {
                result = dst->appendCodeWScope(
                    fieldName,
                    bsonElem.codeWScopeCode(),
                    bsonElem.codeWScopeScopeData());
                break;
            }
            case NumberInt: {
                result = dst->appendInt(fieldName, bsonElem.Int());
                break;
            }
            case Timestamp: {
                result = dst->appendTS(fieldName, bsonElem.timestampTime().millis);
                break;
            }
            case NumberLong: {
                result = dst->appendLong(fieldName, bsonElem.Long());
                break;
            }
            case MaxKey: {
                result = dst->appendMaxKey(fieldName);
                break;
            }
            default: {
            }
            }

            if (!result.isOK())
                break;
        }

        return result;
    }


    //
    // BSONBuilder
    //

    void BSONBuilder::buildFromElement(Element src, BSONObjBuilder* dst) {
        switch (src.type()) {
        case MinKey: {
            dst->appendMinKey(src.fieldName());
            break;
        }
        case EOO: {
            break;
        }
        case NumberDouble: {
            dst->appendNumber(src.fieldName(), src.getDoubleValue());
            break;
        }
        case String: {
            dst->append(src.fieldName(), src.getStringValue());
            break;
        }
        case Object: {
            BSONObjBuilder subBuilder(dst->subobjStart(src.fieldName()));
            BSONBuilder::build(src, &subBuilder);
            break;
        }
        case Array: {
            BSONObjBuilder subBuilder(dst->subarrayStart(src.fieldName()));
            SiblingIterator arrayIt = src.children();
            for (uint32_t n=0; !arrayIt.done(); ++arrayIt,++n) {
                Element e0 = *arrayIt;
                ostringstream oss; oss << "" << n;
                e0.rename(oss.str());
                BSONBuilder::buildFromElement(e0, &subBuilder);
            }
            break;
        }
        case BinData: {
            uint32_t len(0);
            BinDataType subType(mongo::BinDataGeneral);
            dst->appendBinData(src.fieldName(), len, subType, src.getStringValue());
            break;
        }
        case Undefined: {
            dst->appendUndefined(src.fieldName());
            break;
        }
        case jstOID: {
            OID oid = src.getOIDValue();
            dst->appendOID(src.fieldName(), &oid);
            break;
        }
        case Bool: {
            dst->appendBool(src.fieldName(), src.getBoolValue());
            break;
        }
        case Date: {
            dst->appendDate(src.fieldName(), src.getDateValue());
            break;
        }
        case jstNULL: {
            dst->appendNull(src.fieldName());
            break;
        }
        case RegEx: {
            string re("");
            Status err = src.regex(&re);
            if (err.code() != ErrorCodes::OK) {
                std::cout << __TRACE__ << " : [Debug] bad regex : " << src << std:: endl;
                break;
            }
            string flags("");
            err = src.regexFlags(&flags);
            if (err.code() != ErrorCodes::OK) {
                std::cout << __TRACE__ << " : [Debug] bad regex flags : " << src << std:: endl;
                break;
            }
            dst->appendRegex(src.fieldName(), re, flags);
            break;
        }
        case DBRef: {
            string ns("");
            Status err = src.dbrefNS(&ns);
            if (err.code() != ErrorCodes::OK) {
                std::cout << __TRACE__ << " : [Debug] bad dbref ns: " << src << std:: endl;
                break;
            }
            string oidStr("");
            err = src.dbrefOID(&oidStr);
            if (err.code() != ErrorCodes::OK) {
                std::cout << __TRACE__ << " : [Debug] bad dbref oid: " << src << std:: endl;
                break;
            }
            mongo::OID oid(oidStr);
            dst->appendDBRef(src.fieldName(), ns, oid);
            break;
        }

        case Code: {
            dst->appendCode(src.fieldName(), src.getStringValue());
            break;
        }
        case Symbol: {
            dst->appendSymbol(src.fieldName(), src.getStringValue());
            break;
        }
        case CodeWScope: {
            string code("");
            Status err = src.codeWScopeCode(&code);
            if (err.code() != ErrorCodes::OK) {
                std::cout << __TRACE__ << " : [Debug] bad codeWScope code : " << src << std:: endl;
                break;
            }
            string scope("");
            err = src.codeWScopeScope(&scope);
            if (err.code() != ErrorCodes::OK) {
                std::cout << __TRACE__ << " : [Debug] bad codeWScope scope: " << src << std:: endl;
                break;
            }
            dst->appendCode(code, scope);
            break;
        }

        case NumberInt: {
            dst->appendNumber(src.fieldName(), src.getIntValue());
            break;
        }
        case Timestamp: {
            dst->appendTimeT(src.fieldName(), src.getLongValue());
            break;
        }
        case NumberLong: {
            dst->appendNumber(src.fieldName(), src.getLongValue());
            break;
        }
        case MaxKey: {
            dst->appendMaxKey(src.fieldName());
            break;
        }
        default: {
        }
        }
    }

    void BSONBuilder::build(Element src, BSONObjBuilder* dst) {

        SiblingIterator it(src);

        for (; !it.done(); ++it) {
            Element elem = *it;
            BSONBuilder::buildFromElement(elem, dst);
        }
    }

} // namespace mutablebson
} // namespace mongo
