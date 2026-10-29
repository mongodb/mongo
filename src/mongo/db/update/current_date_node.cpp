// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/update/current_date_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
constexpr std::string_view kType = "$type"sv;
constexpr std::string_view kDate = "date"sv;
constexpr std::string_view kTimestamp = "timestamp"sv;

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
                    std::string_view valueString = elem.valueStringData();
                    if (valueString == kDate) {
                        _typeIsDate = true;
                        foundValidType = true;
                    } else if (valueString == kTimestamp) {
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

BSONObj CurrentDateNode::operatorValue(const query_shape::SerializationOptions& opts) const {
    // We do not need to do any special serialization here, because type is simply an enum with only
    // two possible values.
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
