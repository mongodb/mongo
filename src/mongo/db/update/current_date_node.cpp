/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/update/current_date_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
constexpr StringData kType = "$type"_sd;
constexpr StringData kDate = "date"_sd;
constexpr StringData kTimestamp = "timestamp"_sd;

void setValue(ServiceContext* service, mutablebson::Element* element, bool typeIsDate) {
    if (typeIsDate) {
        invariant(element->setValueDate(mongo::Date_t::now()));
    } else {
        invariant(element->setValueTimestamp(
            VectorClockMutable::get(service)->tickClusterTime(1).asTimestamp()));
    }
}
}  // namespace

Status CurrentDateNode::init(BSONElement modExpr,
                             const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    if (modExpr.type() == BSONType::boolean) {
        _typeIsDate = true;
    } else if (modExpr.type() == BSONType::object) {
        auto foundValidType = false;
        for (auto&& elem : modExpr.Obj()) {
            if (elem.fieldNameStringData() == kType) {
                if (elem.type() == BSONType::string) {
                    if (elem.valueStringData() == kDate) {
                        _typeIsDate = true;
                        foundValidType = true;
                    } else if (elem.valueStringData() == kTimestamp) {
                        _typeIsDate = false;
                        foundValidType = true;
                    }
                }
            } else {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Unrecognized $currentDate option: "
                                            << elem.fieldNameStringData());
            }
        }

        if (!foundValidType) {
            return Status(ErrorCodes::BadValue,
                          "The '$type' string field is required "
                          "to be 'date' or 'timestamp': "
                          "{$currentDate: {field : {$type: 'date'}}}");
        }
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << typeName(modExpr.type())
                                    << " is not valid type for $currentDate."
                                       " Please use a boolean ('true')"
                                       " or a $type expression ({$type: 'timestamp/date'}).");
    }

    _service = expCtx->getOperationContext()->getServiceContext();

    return Status::OK();
}

ModifierNode::ModifyResult CurrentDateNode::updateExistingElement(
    mutablebson::Element* element, const FieldRef& elementPath) const {
    setValue(_service, element, _typeIsDate);
    return ModifyResult::kNormalUpdate;
}

void CurrentDateNode::setValueForNewElement(mutablebson::Element* element) const {
    setValue(_service, element, _typeIsDate);
}

BSONObj CurrentDateNode::operatorValue() const {
    BSONObjBuilder bob;
    {
        BSONObjBuilder subBuilder(bob.subobjStart(""));
        {
            if (_typeIsDate)
                subBuilder << kType << kDate;
            else
                subBuilder << kType << kTimestamp;
        }
    }
    return bob.obj();
}

}  // namespace mongo
