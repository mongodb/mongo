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

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_builder.h"

#include <sstream>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"

namespace mongo {
namespace mutablebson {

    //
    // ElementBuilder
    //

    Status ElementBuilder::parse(const BSONObj& src, Element* dst) {
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
                result = ElementBuilder::parse(bsonElem.Obj(), &e0);
                if (result.isOK())
                    result = dst->addChild(e0);
                break;
            }
            case Array: {
                Element e0 = doc.makeArrayElement(fieldName);
                result = ElementBuilder::parse(bsonElem.Obj(), &e0);
                if (result.isOK())
                    result = dst->addChild(e0);
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

        const StringData srcFieldName = src.getFieldName();

        switch (src.type()) {
        case MinKey: {
            dst->appendMinKey(srcFieldName);
            break;
        }
        case EOO: {
            break;
        }
        case NumberDouble: {
            dst->appendNumber(srcFieldName, src.getDoubleValue());
            break;
        }
        case String: {
            dst->append(srcFieldName, src.getStringValue());
            break;
        }
        case Object: {
            BSONObjBuilder subBuilder(dst->subobjStart(srcFieldName));
            BSONBuilder::build(src, &subBuilder);
            subBuilder.doneFast();
            break;
        }
        case Array: {
            BSONObjBuilder subBuilder(dst->subarrayStart(srcFieldName));
            SiblingIterator arrayIt = src.children();
            for (uint32_t n=0; !arrayIt.done(); ++arrayIt,++n) {
                Element e0 = *arrayIt;
                ostringstream oss; oss << "" << n;
                e0.rename(oss.str());
                BSONBuilder::buildFromElement(e0, &subBuilder);
            }
            subBuilder.doneFast();
            break;
        }
        case BinData: {
            uint32_t len(0);
            BinDataType subType(mongo::BinDataGeneral);
            dst->appendBinData(srcFieldName, len, subType, src.getStringValue());
            break;
        }
        case Undefined: {
            dst->appendUndefined(srcFieldName);
            break;
        }
        case jstOID: {
            OID oid = src.getOIDValue();
            dst->appendOID(srcFieldName, &oid);
            break;
        }
        case Bool: {
            dst->appendBool(srcFieldName, src.getBoolValue());
            break;
        }
        case Date: {
            dst->appendDate(srcFieldName, src.getDateValue());
            break;
        }
        case jstNULL: {
            dst->appendNull(srcFieldName);
            break;
        }
        case RegEx: {
            string re("");
            Status err = src.regex(&re);
            if (err.code() != ErrorCodes::OK) {
                break;
            }
            string flags("");
            err = src.regexFlags(&flags);
            if (err.code() != ErrorCodes::OK) {
                break;
            }
            dst->appendRegex(srcFieldName, re, flags);
            break;
        }
        case DBRef: {
            string ns("");
            Status err = src.dbrefNS(&ns);
            if (err.code() != ErrorCodes::OK) {
                break;
            }
            string oidStr("");
            err = src.dbrefOID(&oidStr);
            if (err.code() != ErrorCodes::OK) {
                break;
            }
            mongo::OID oid(oidStr);
            dst->appendDBRef(srcFieldName, ns, oid);
            break;
        }

        case Code: {
            dst->appendCode(srcFieldName, src.getStringValue());
            break;
        }
        case Symbol: {
            dst->appendSymbol(srcFieldName, src.getStringValue());
            break;
        }
        case CodeWScope: {
            string code("");
            Status err = src.codeWScopeCode(&code);
            if (err.code() != ErrorCodes::OK) {
                break;
            }
            string scope("");
            err = src.codeWScopeScope(&scope);
            if (err.code() != ErrorCodes::OK) {
                break;
            }
            dst->appendCode(code, scope);
            break;
        }

        case NumberInt: {
            dst->appendNumber(srcFieldName, src.getIntValue());
            break;
        }
        case Timestamp: {
            dst->appendTimeT(srcFieldName, src.getLongValue());
            break;
        }
        case NumberLong: {
            dst->appendNumber(srcFieldName, static_cast<long long>(src.getLongValue()));
            break;
        }
        case MaxKey: {
            dst->appendMaxKey(srcFieldName);
            break;
        }
        default: {
        }
        }
    }

    void BSONBuilder::build(Element src, BSONObjBuilder* dst) {

        SiblingIterator it = src.children();

        for (; !it.done(); ++it) {
            Element elem = *it;
            BSONBuilder::buildFromElement(elem, dst);
        }
    }

} // namespace mutablebson
} // namespace mongo
