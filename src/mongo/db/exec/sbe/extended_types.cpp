/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/base/compare_numbers.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/in_list.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/sort_spec.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/matcher/in_list_data.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre_util.h"

namespace mongo::sbe {
using TypeTags = value::TypeTags;
using Value = value::Value;
using ExtendedTypeOps = value::ExtendedTypeOps;

namespace {
struct PcreRegexOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& regex = *value::getPcreRegexView(val);
        auto copy = std::make_unique<pcre::Regex>(regex);
        return {TypeTags::pcreRegex, value::bitcastFrom<pcre::Regex*>(copy.release())};
    }

    static void release(Value val) {
        delete value::getPcreRegexView(val);
    }

    static std::string print(Value val) {
        std::stringstream ss;
        auto& regex = *value::getPcreRegexView(val);
        ss << "PcreRegex(/" << regex.pattern() << "/" << pcre_util::optionsToFlags(regex.options())
           << ")";
        return ss.str();
    }

    static size_t getApproxSize(Value val) {
        return value::getPcreRegexView(val)->codeSize();
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct JsFunctionOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& jsFunction = *value::getJsFunctionView(val);
        auto copy = value::bitcastFrom<JsFunction*>(new JsFunction(jsFunction));
        return {TypeTags::jsFunction, copy};
    }

    static void release(Value val) {
        delete value::getJsFunctionView(val);
    }

    static std::string print(Value val) {
        return "jsFunction";
    }

    static size_t getApproxSize(Value val) {
        return value::getJsFunctionView(val)->getApproximateSize();
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct ShardFiltererOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& filterer = *value::getShardFiltererView(val);
        auto copy = value::bitcastFrom<ShardFilterer*>(filterer.clone().release());
        return {TypeTags::shardFilterer, copy};
    }

    static void release(Value val) {
        delete value::getShardFiltererView(val);
    }

    static std::string print(Value val) {
        return "ShardFilterer";
    }

    static size_t getApproxSize(Value val) {
        return value::getShardFiltererView(val)->getApproximateSize();
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct FtsMatcherOps {
    using FTSMatcher = fts::FTSMatcher;

    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& fts = *value::getFtsMatcherView(val);
        auto copy = value::bitcastFrom<FTSMatcher*>(new FTSMatcher(fts.query(), fts.spec()));
        return {TypeTags::ftsMatcher, copy};
    }

    static void release(Value val) {
        delete value::getFtsMatcherView(val);
    }

    static std::string print(Value val) {
        std::stringstream ss;
        auto printer = value::ValuePrinters::make(ss, PrintOptions());

        ss << "FtsMatcher(";
        printer.writeObjectToStream(value::getFtsMatcherView(val)->query().toBSON());
        ss << ")";
        return ss.str();
    }

    static size_t getApproxSize(Value val) {
        return value::getFtsMatcherView(val)->getApproximateSize();
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct SortSpecOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& ss = *value::getSortSpecView(val);
        auto copy = value::bitcastFrom<SortSpec*>(new SortSpec(ss));
        return {TypeTags::sortSpec, copy};
    }

    static void release(Value val) {
        delete value::getSortSpecView(val);
    }

    static std::string print(Value val) {
        std::stringstream ss;
        auto printer = value::ValuePrinters::make(ss, PrintOptions());

        ss << "SortSpec(";
        printer.writeObjectToStream(value::getSortSpecView(val)->getPattern());
        ss << ")";
        return ss.str();
    }

    static size_t getApproxSize(Value val) {
        return value::getSortSpecView(val)->getApproximateSize();
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct MakeObjSpecOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& spec = *value::getMakeObjSpecView(val);
        auto copy = value::bitcastFrom<MakeObjSpec*>(new MakeObjSpec(spec));
        return {TypeTags::makeObjSpec, copy};
    }

    static void release(Value val) {
        delete value::getMakeObjSpecView(val);
    }

    static std::string print(Value val) {
        std::stringstream ss;
        ss << "MakeObjSpec(" << value::getMakeObjSpecView(val)->toString() << ")";
        return ss.str();
    }

    static size_t getApproxSize(Value val) {
        return value::getMakeObjSpecView(val)->getApproximateSize();
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct IndexBoundsOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto& bounds = *value::getIndexBoundsView(val);
        auto copy = value::bitcastFrom<IndexBounds*>(new IndexBounds(bounds));
        return {TypeTags::indexBounds, copy};
    }

    static void release(Value val) {
        delete value::getIndexBoundsView(val);
    }

    static std::string print(Value val) {
        // When calling toString() we don't know if the index has a non-simple collation or
        // not. Passing false could produce invalid UTF-8, which is not acceptable when we are
        // going to put the resulting string into a BSON object and return it across the wire.
        // While passing true may be misleading in cases when the index has no collation, it is
        // safer to do so.
        std::stringstream ss;
        auto printer = value::ValuePrinters::make(ss, PrintOptions());

        ss << "IndexBounds(";
        printer.writeStringDataToStream(value::getIndexBoundsView(val)->toString(true));
        ss << ")";
        return ss.str();
    }

    static size_t getApproxSize(Value val) {
        return size_estimator::estimate(*value::getIndexBoundsView(val));
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};

struct InListOps {
    static std::pair<TypeTags, Value> makeCopy(Value val) {
        auto inListCopy = new InList(*value::getInListView(val));
        return {TypeTags::inList, value::bitcastFrom<InList*>(inListCopy)};
    }

    static void release(Value val) {
        delete value::getInListView(val);
    }

    static std::string print(Value val) {
        std::stringstream ss;
        ss << "InList(";
        value::getInListView(val)->writeToStream(ss);
        ss << ")";
        return ss.str();
    }

    static size_t getApproxSize(Value val) {
        return 0;
    }

    static constexpr ExtendedTypeOps typeOps{&makeCopy, &release, &print, &getApproxSize};
};
}  // namespace

MONGO_INITIALIZER(ExtendedSbeTypes)(InitializerContext* context) {
    value::registerExtendedTypeOps(TypeTags::pcreRegex, &PcreRegexOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::jsFunction, &JsFunctionOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::shardFilterer, &ShardFiltererOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::ftsMatcher, &FtsMatcherOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::sortSpec, &SortSpecOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::makeObjSpec, &MakeObjSpecOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::indexBounds, &IndexBoundsOps::typeOps);
    value::registerExtendedTypeOps(TypeTags::inList, &InListOps::typeOps);
}
}  // namespace mongo::sbe
