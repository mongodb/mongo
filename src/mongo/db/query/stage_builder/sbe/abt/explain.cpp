/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/abt/explain.h"

#include <cstddef>
#include <cstdint>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/containers.h"
#include "mongo/db/query/stage_builder/sbe/abt/strong_alias.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>


namespace mongo::abt {

bool constexpr operator<(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) < static_cast<int>(v2);
}
bool constexpr operator<=(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) <= static_cast<int>(v2);
}
bool constexpr operator>(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) > static_cast<int>(v2);
}
bool constexpr operator>=(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) >= static_cast<int>(v2);
}

static constexpr ExplainVersion kDefaultExplainVersion = ExplainVersion::V2;

enum class CommandType { Indent, Unindent, AddLine };

struct CommandStruct {
    CommandStruct() = default;
    CommandStruct(const CommandType type, std::string str) : _type(type), _str(std::move(str)) {}

    CommandType _type;
    std::string _str;
};

using CommandVector = std::vector<CommandStruct>;

/**
 * Helper class for building indented, multiline strings.
 *
 * The main operations it supports are:
 *   - Print a single value, of any type that supports '<<' to std::ostream.
 *   - Indent/unindent, and add newlines.
 *   - Print another ExplainPrinterImpl, preserving its 2D layout.
 *
 * Being able to print another whole printer makes it easy to build these 2D strings
 * bottom-up, without passing around a std::ostream. It also allows displaying
 * child elements in a different order than they were visited.
 */
template <const ExplainVersion version = kDefaultExplainVersion>
class ExplainPrinterImpl {
public:
    ExplainPrinterImpl()
        : _cmd(),
          _os(),
          _osDirty(false),
          _indentCount(0),
          _childrenRemaining(0),
          _inlineNextChild(false),
          _cmdInsertPos(-1) {}

    ExplainPrinterImpl(const ExplainPrinterImpl& other) = delete;
    ExplainPrinterImpl& operator=(const ExplainPrinterImpl& other) = delete;

    explicit ExplainPrinterImpl(const std::string& initialStr) : ExplainPrinterImpl() {
        print(initialStr);
    }

    ExplainPrinterImpl(ExplainPrinterImpl&& other) noexcept
        : _cmd(std::move(other._cmd)),
          _os(std::move(other._os)),
          _osDirty(other._osDirty),
          _indentCount(other._indentCount),
          _childrenRemaining(other._childrenRemaining),
          _inlineNextChild(other._inlineNextChild),
          _cmdInsertPos(other._cmdInsertPos) {}

    template <class T>
    ExplainPrinterImpl& print(const T& t) {
        _os << t;
        _osDirty = true;
        return *this;
    }

    ExplainPrinterImpl& print(StringData s) {
        print(s.empty() ? "<empty>" : s.data());
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongStringAlias<TagType>& t) {
        print(t.value().empty() ? "<empty>" : t.value());
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongDoubleAlias<TagType>& t) {
        print(t._value);
        return *this;
    }

    /**
     * Here and below: "other" printer(s) may be siphoned out.
     */
    ExplainPrinterImpl& print(ExplainPrinterImpl& other) {
        return print(other, false /*singleLevel*/);
    }

    template <class P>
    ExplainPrinterImpl& printSingleLevel(P& other, const std::string& singleLevelSpacer = " ") {
        return print(other, true /*singleLevel*/, singleLevelSpacer);
    }

    ExplainPrinterImpl& printAppend(ExplainPrinterImpl& other) {
        // Ignore append
        return print(other);
    }

    ExplainPrinterImpl& print(std::vector<ExplainPrinterImpl>& other) {
        for (auto&& element : other) {
            print(element);
        }
        return *this;
    }

    ExplainPrinterImpl& printAppend(std::vector<ExplainPrinterImpl>& other) {
        // Ignore append.
        return print(other);
    }

    ExplainPrinterImpl& setChildCount(const size_t childCount, const bool noInline = false) {
        _childrenRemaining = childCount;
        indent("");
        for (int i = 0; i < _childrenRemaining - 1; i++) {
            indent("|");
        }
        return *this;
    }

    ExplainPrinterImpl& maybeReverse() {
        _cmdInsertPos = _cmd.size();
        return *this;
    }

    ExplainPrinterImpl& fieldName(const std::string& name,
                                  const ExplainVersion minVersion = ExplainVersion::V2,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        if (minVersion <= version && maxVersion >= version) {
            print(name);
            print(": ");
        }
        return *this;
    }

    ExplainPrinterImpl& separator(const std::string& separator) {
        return print(separator);
    }

    std::string str() {
        newLine();

        std::ostringstream os;
        std::vector<std::string> linePrefix;

        for (const auto& cmd : _cmd) {
            switch (cmd._type) {
                case CommandType::Indent:
                    linePrefix.push_back(cmd._str);
                    break;

                case CommandType::Unindent: {
                    linePrefix.pop_back();
                    break;
                }

                case CommandType::AddLine: {
                    for (const std::string& element : linePrefix) {
                        if (!element.empty()) {
                            os << element << "   ";
                        }
                    }
                    os << cmd._str << "\n";
                    break;
                }

                default: {
                    MONGO_UNREACHABLE;
                }
            }
        }

        return os.str();
    }

    /**
     * Ends the current line, if there is one. Repeated calls do not create
     * blank lines.
     */
    void newLine() {
        if (!_osDirty) {
            return;
        }
        const std::string& str = _os.str();
        _cmd.emplace_back(CommandType::AddLine, str);
        _os.str("");
        _os.clear();
        _osDirty = false;
    }

    const CommandVector& getCommands() const {
        return _cmd;
    }

private:
    template <class P>
    ExplainPrinterImpl& print(P& other,
                              const bool singleLevel,
                              const std::string& singleLevelSpacer = " ") {
        CommandVector toAppend;
        if (_cmdInsertPos >= 0) {
            toAppend = CommandVector(_cmd.cbegin() + _cmdInsertPos, _cmd.cend());
            _cmd.resize(static_cast<size_t>(_cmdInsertPos));
        }

        const bool hadChildrenRemaining = _childrenRemaining > 0;
        if (hadChildrenRemaining) {
            _childrenRemaining--;
        }
        other.newLine();

        if (singleLevel) {
            uassert(6624071, "Unexpected dirty status", _osDirty);

            bool first = true;
            for (const auto& element : other.getCommands()) {
                if (element._type == CommandType::AddLine) {
                    if (first) {
                        first = false;
                    } else {
                        _os << singleLevelSpacer;
                    }
                    _os << element._str;
                }
            }
        } else if (_inlineNextChild) {
            _inlineNextChild = false;
            // Print 'other' without starting a new line.
            // Embed its first line into our current one, and keep the rest of its commands.
            bool first = true;
            for (const CommandStruct& element : other.getCommands()) {
                if (first && element._type == CommandType::AddLine) {
                    _os << singleLevelSpacer << element._str;
                } else {
                    newLine();
                    _cmd.push_back(element);
                }
                first = false;
            }
        } else {
            newLine();
            // If 'hadChildrenRemaining' then 'other' represents a child of 'this', which means
            // there was a prior call to setChildCount() that added indentation for it.
            // If '! hadChildrenRemaining' then create indentation for it now.
            if (!hadChildrenRemaining) {
                indent();
            }
            for (const auto& element : other.getCommands()) {
                _cmd.push_back(element);
            }
            unIndent();
        }

        if (_cmdInsertPos >= 0) {
            std::copy(toAppend.cbegin(), toAppend.cend(), std::back_inserter(_cmd));
        }

        return *this;
    }

    void indent(std::string s = " ") {
        newLine();
        _indentCount++;
        _cmd.emplace_back(CommandType::Indent, std::move(s));
    }

    void unIndent() {
        newLine();
        _indentCount--;
        _cmd.emplace_back(CommandType::Unindent, "");
    }

    // Holds completed lines, and indent/unIndent commands.
    // When '_cmdInsertPos' is nonnegative, some of these lines and commands belong
    // after the currently-being-built line.
    CommandVector _cmd;
    // Holds the incomplete line currently being built. Once complete this will become the last
    // line, unless '_cmdInsertPos' is nonnegative.
    std::ostringstream _os;
    // True means we have an incomplete line in '_os'.
    // Once the line is completed with newLine(), this flag is false until
    // we begin building a new one with print().
    bool _osDirty;
    int _indentCount;
    int _childrenRemaining;
    bool _inlineNextChild;
    // When nonnegative, indicates the insertion point where completed lines
    // should be added to '_cmd'. -1 means completed lines will be added at the end.
    int _cmdInsertPos;
};

template <>
class ExplainPrinterImpl<ExplainVersion::V3> {
    static constexpr ExplainVersion version = ExplainVersion::V3;

public:
    ExplainPrinterImpl() {
        reset();
    }

    ~ExplainPrinterImpl() {
        if (_initialized) {
            releaseValue(_tag, _val);
        }
    }

    ExplainPrinterImpl(const ExplainPrinterImpl& other) = delete;
    ExplainPrinterImpl& operator=(const ExplainPrinterImpl& other) = delete;

    ExplainPrinterImpl(ExplainPrinterImpl&& other) noexcept {
        _nextFieldName = std::move(other._nextFieldName);
        _initialized = other._initialized;
        _canAppend = other._canAppend;
        _tag = other._tag;
        _val = other._val;
        _fieldNameSet = std::move(other._fieldNameSet);

        other.reset();
    }

    explicit ExplainPrinterImpl(const std::string& nodeName) : ExplainPrinterImpl() {
        fieldName("nodeType").print(nodeName);
    }

    auto moveValue() {
        auto result = std::pair<sbe::value::TypeTags, sbe::value::Value>(_tag, _val);
        reset();
        return result;
    }

    ExplainPrinterImpl& print(const bool v) {
        addValue(sbe::value::TypeTags::Boolean, v);
        return *this;
    }

    ExplainPrinterImpl& print(const int64_t v) {
        addValue(sbe::value::TypeTags::NumberInt64, sbe::value::bitcastFrom<int64_t>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const int32_t v) {
        addValue(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const size_t v) {
        addValue(sbe::value::TypeTags::NumberInt64, sbe::value::bitcastFrom<size_t>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const double v) {
        addValue(sbe::value::TypeTags::NumberDouble, sbe::value::bitcastFrom<double>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const std::pair<sbe::value::TypeTags, sbe::value::Value> v) {
        if (sbe::value::tagToType(v.first) == BSONType::eoo &&
            v.first != sbe::value::TypeTags::Nothing) {
            if (v.first == sbe::value::TypeTags::makeObjSpec) {
                // We want to append a stringified version of MakeObjSpec to explain here.
                auto [mosTag, mosVal] =
                    sbe::value::makeNewString(sbe::value::getMakeObjSpecView(v.second)->toString());
                addValue(mosTag, mosVal);
            } else if (v.first == sbe::value::TypeTags::pcreRegex) {
                // We want to append the pattern of the regular expression to explain here.
                auto [regexTag, regexVal] =
                    sbe::value::makeNewString(sbe::value::getPcreRegexView(v.second)->pattern());
                addValue(regexTag, regexVal);
            } else if (v.first == sbe::value::TypeTags::timeZone) {
                // We want to append the name of the timezone expression to explain here.
                auto [tzTag, tzVal] =
                    sbe::value::makeNewString(sbe::value::getTimeZoneView(v.second)->toString());
                addValue(tzTag, tzVal);
            } else {
                // Extended types need to implement their own explain, since we can't directly
                // convert them to bson.
                MONGO_UNREACHABLE_TASSERT(7936708);
            }

        } else {
            auto [tag, val] = sbe::value::copyValue(v.first, v.second);
            addValue(tag, val);
        }

        return *this;
    }

    ExplainPrinterImpl& print(const std::string& s) {
        printStringInternal(s);
        return *this;
    }

    ExplainPrinterImpl& print(StringData s) {
        printStringInternal(s);
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongStringAlias<TagType>& s) {
        printStringInternal(s.value());
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongDoubleAlias<TagType>& v) {
        return print(v._value);
    }

    ExplainPrinterImpl& print(const char* s) {
        return print(static_cast<std::string>(s));
    }

    /**
     * Here and below: "other" printer(s) may be siphoned out.
     */
    ExplainPrinterImpl& print(ExplainPrinterImpl& other) {
        return print(other, false /*append*/);
    }

    ExplainPrinterImpl& printSingleLevel(ExplainPrinterImpl& other,
                                         const std::string& /*singleLevelSpacer*/ = " ") {
        // Ignore single level.
        return print(other);
    }

    ExplainPrinterImpl& printAppend(ExplainPrinterImpl& other) {
        return print(other, true /*append*/);
    }

    ExplainPrinterImpl& print(std::vector<ExplainPrinterImpl>& other) {
        return print(other, false /*append*/);
    }

    ExplainPrinterImpl& printAppend(std::vector<ExplainPrinterImpl>& other) {
        return print(other, true /*append*/);
    }

    ExplainPrinterImpl& setChildCount(const size_t /*childCount*/) {
        // Ignored.
        return *this;
    }

    ExplainPrinterImpl& maybeReverse() {
        // Ignored.
        return *this;
    }

    template <size_t N>
    ExplainPrinterImpl& fieldName(const char (&name)[N],
                                  const ExplainVersion minVersion = ExplainVersion::V2,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        fieldNameInternal(name, minVersion, maxVersion);
        return *this;
    }

    ExplainPrinterImpl& fieldName(const std::string& name,
                                  const ExplainVersion minVersion = ExplainVersion::V2,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        fieldNameInternal(name, minVersion, maxVersion);
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& fieldName(const StrongStringAlias<TagType>& name,
                                  const ExplainVersion minVersion = ExplainVersion::V2,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        fieldNameInternal(std::string{name.value()}, minVersion, maxVersion);
        return *this;
    }

    ExplainPrinterImpl& separator(const std::string& /*separator*/) {
        // Ignored.
        return *this;
    }

private:
    ExplainPrinterImpl& printStringInternal(StringData s) {
        auto [tag, val] = sbe::value::makeNewString(s);
        addValue(tag, val);
        return *this;
    }

    ExplainPrinterImpl& fieldNameInternal(const std::string& name,
                                          const ExplainVersion minVersion,
                                          const ExplainVersion maxVersion) {
        if (minVersion <= version && maxVersion >= version) {
            _nextFieldName = name;
        }
        return *this;
    }

    ExplainPrinterImpl& print(ExplainPrinterImpl& other, const bool append) {
        auto [tag, val] = other.moveValue();
        addValue(tag, val, append);
        if (append) {
            sbe::value::releaseValue(tag, val);
        }
        return *this;
    }

    ExplainPrinterImpl& print(std::vector<ExplainPrinterImpl>& other, const bool append) {
        auto [tag, val] = sbe::value::makeNewArray();
        sbe::value::Array* arr = sbe::value::getArrayView(val);
        for (auto&& element : other) {
            auto [tag1, val1] = element.moveValue();
            arr->push_back(tag1, val1);
        }
        addValue(tag, val, append);
        return *this;
    }

    void addValue(sbe::value::TypeTags tag, sbe::value::Value val, const bool append = false) {
        if (!_initialized) {
            _initialized = true;
            _canAppend = _nextFieldName.has_value();
            if (_canAppend) {
                std::tie(_tag, _val) = sbe::value::makeNewObject();
            } else {
                _tag = tag;
                _val = val;
                return;
            }
        }

        if (!_canAppend) {
            uasserted(6624072, "Cannot append to scalar");
            return;
        }

        if (append) {
            uassert(6624073, "Field name is not set", !_nextFieldName.has_value());
            uassert(6624349,
                    "Other printer does not contain Object",
                    tag == sbe::value::TypeTags::Object);
            sbe::value::Object* obj = sbe::value::getObjectView(val);
            for (size_t i = 0; i < obj->size(); i++) {
                const auto field = obj->getAt(i);
                auto [fieldTag, fieldVal] = sbe::value::copyValue(field.first, field.second);
                addField(obj->field(i), fieldTag, fieldVal);
            }
        } else {
            tassert(6751700, "Missing field name to serialize", _nextFieldName);
            addField(*_nextFieldName, tag, val);
            _nextFieldName = boost::none;
        }
    }

    void addField(const std::string& fieldName, sbe::value::TypeTags tag, sbe::value::Value val) {
        uassert(6624075, "Duplicate field name", _fieldNameSet.insert(fieldName).second);
        sbe::value::getObjectView(_val)->push_back(fieldName, tag, val);
    }

    void reset() {
        _nextFieldName = boost::none;
        _initialized = false;
        _canAppend = false;
        _tag = sbe::value::TypeTags::Nothing;
        _val = 0;
        _fieldNameSet.clear();
    }

    // Cannot assume empty means non-existent, so use optional<>.
    boost::optional<std::string> _nextFieldName;
    bool _initialized;
    bool _canAppend;
    sbe::value::TypeTags _tag;
    sbe::value::Value _val;
    // For debugging.
    opt::unordered_set<std::string> _fieldNameSet;
};

template <const ExplainVersion version = kDefaultExplainVersion>
class ExplainGeneratorTransporter {
public:
    using ExplainPrinter = ExplainPrinterImpl<version>;

    static void printBooleanFlag(ExplainPrinter& printer,
                                 const std::string& name,
                                 const bool flag,
                                 const bool addComma = true) {
        if constexpr (version < ExplainVersion::V3) {
            if (flag) {
                if (addComma) {
                    printer.print(", ");
                }
                printer.print(name);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            printer.fieldName(name).print(flag);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    static void printDirectToParentHelper(const bool directToParent,
                                          ExplainPrinter& parent,
                                          std::function<void(ExplainPrinter& printer)> fn) {
        if (directToParent) {
            fn(parent);
        } else {
            ExplainPrinter printer;
            fn(printer);
            parent.printAppend(printer);
        }
    }

    template <class T>
    static void printProjectionsUnordered(ExplainPrinter& printer, const T& projections) {
        if constexpr (version < ExplainVersion::V3) {
            if (!projections.empty()) {
                printer.separator("{");
                bool first = true;
                for (const ProjectionName& projectionName : projections) {
                    if (first) {
                        first = false;
                    } else {
                        printer.separator(", ");
                    }
                    printer.print(projectionName);
                }
                printer.separator("}");
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const ProjectionName& projectionName : projections) {
                ExplainPrinter local;
                local.print(projectionName);
                printers.push_back(std::move(local));
            }
            printer.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    /**
     * Nodes
     */
    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const References& references,
                             std::vector<ExplainPrinter> inResults) {
        ExplainPrinter printer;
        if constexpr (version < ExplainVersion::V3) {
            // The ref block is redundant for V1 and V2. We typically explain the references in the
            // blocks ([]) of the individual elements.
        } else if constexpr (version == ExplainVersion::V3) {
            printer.printAppend(inResults);
        } else {
            MONGO_UNREACHABLE;
        }
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const ExpressionBinder& binders,
                             std::vector<ExplainPrinter> inResults) {
        ExplainPrinter printer;
        if constexpr (version < ExplainVersion::V3) {
            // The bind block is redundant for V1-V2 type explains, as the bound projections can be
            // inferred from the field projection map; so here we print nothing.
            return printer;
        } else if constexpr (version == ExplainVersion::V3) {
            std::map<ProjectionName, ExplainPrinter> ordered;
            for (size_t idx = 0; idx < inResults.size(); ++idx) {
                ordered.emplace(binders.names()[idx], std::move(inResults[idx]));
            }
            printer.separator("BindBlock:");
            for (auto& [name, child] : ordered) {
                printer.separator(" ").fieldName(name).print(child);
            }
        } else {
            MONGO_UNREACHABLE;
        }
        return printer;
    }

    /**
     * Expressions
     */
    ExplainPrinter transport(const ABT::reference_type /*n*/, const Blackhole& expr) {
        ExplainPrinter printer("Blackhole");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const Constant& expr) {
        ExplainPrinter printer("Const");
        printer.separator(" [").fieldName("tag", ExplainVersion::V3);

        if (version == ExplainVersion::V3) {
            std::stringstream ss;
            ss << expr.get().first;
            std::string tagAsString = ss.str();
            printer.print(tagAsString);
        }

        printer.fieldName("value", ExplainVersion::V3).print(expr.get()).separator("]");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const Variable& expr) {
        ExplainPrinter printer("Variable");
        printer.separator(" [")
            .fieldName("name", ExplainVersion::V3)
            .print(expr.name())
            .separator("]");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const UnaryOp& expr,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("UnaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(toStringData(expr.op()))
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const BinaryOp& expr,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("BinaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(toStringData(expr.op()))
            .separator("]")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("left", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("right", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const NaryOp& expr,
                             std::vector<ExplainPrinter> argResults) {
        ExplainPrinter printer("NaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(toStringData(expr.op()))
            .separator("]")
            .setChildCount(argResults.size())
            .maybeReverse();
        for (size_t i = 0; i < argResults.size(); i++) {
            std::stringstream ss;
            ss << "arg" << i;
            printer.fieldName(ss.str(), ExplainVersion::V3).print(argResults[i]);
        }
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const If& expr,
                             ExplainPrinter condResult,
                             ExplainPrinter thenResult,
                             ExplainPrinter elseResult) {
        ExplainPrinter printer("If");
        printer.separator(" []")
            .setChildCount(3)
            .maybeReverse()
            .fieldName("condition", ExplainVersion::V3)
            .print(condResult)
            .fieldName("then", ExplainVersion::V3)
            .print(thenResult)
            .fieldName("else", ExplainVersion::V3)
            .print(elseResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const Switch& expr,
                             std::vector<ExplainPrinter> argResults) {
        ExplainPrinter printer("Switch");
        printer.separator(" []").setChildCount(argResults.size()).maybeReverse();
        for (size_t i = 0; i < expr.getNumBranches(); i++) {
            std::stringstream ssCond;
            ssCond << "condition" << i;
            std::stringstream ssThen;
            ssThen << "then" << i;
            printer.fieldName(ssCond.str(), ExplainVersion::V3)
                .print(argResults[i * 2])
                .fieldName(ssThen.str(), ExplainVersion::V3)
                .print(argResults[i * 2 + 1]);
        }
        printer.fieldName("else", ExplainVersion::V3).print(argResults.back());
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const Let& expr,
                             ExplainPrinter bindResult,
                             ExplainPrinter exprResult) {
        ExplainPrinter printer("Let");
        printer.separator(" [")
            .fieldName("variable", ExplainVersion::V3)
            .print(expr.varName())
            .separator("]")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("bind", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("expression", ExplainVersion::V3)
            .print(exprResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const MultiLet& expr,
                             std::vector<ExplainPrinter> results) {
        auto numBinds = expr.numBinds();

        ExplainPrinter printer("MultiLet");
        printer.separator(" [");
        for (size_t idx = 0; idx < numBinds; ++idx) {
            std::stringstream ss;
            ss << "variable" << idx;
            printer.fieldName(ss.str(), ExplainVersion::V3).print(expr.varName(idx));
            if (idx < numBinds - 1) {
                printer.separator(", ");
            }
        }
        printer.separator("]").setChildCount(numBinds + 1).maybeReverse();

        for (size_t idx = 0; idx < numBinds; ++idx) {
            std::stringstream ss;
            ss << "bind" << idx;
            printer.fieldName(ss.str(), ExplainVersion::V3).print(results[idx]);
        }

        printer.fieldName("expression", ExplainVersion::V3).print(results.back());

        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const LambdaAbstraction& expr,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("LambdaAbstraction");
        printer.separator(" [")
            .fieldName("variable", ExplainVersion::V3)
            .print(expr.varName())
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const LambdaApplication& expr,
                             ExplainPrinter lambdaResult,
                             ExplainPrinter argumentResult) {
        ExplainPrinter printer("LambdaApplication");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("lambda", ExplainVersion::V3)
            .print(lambdaResult)
            .fieldName("argument", ExplainVersion::V3)
            .print(argumentResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const FunctionCall& expr,
                             std::vector<ExplainPrinter> argResults) {
        ExplainPrinter printer("FunctionCall");
        printer.separator(" [")
            .fieldName("name", ExplainVersion::V3)
            .print(expr.name())
            .separator("]");
        if (!argResults.empty()) {
            printer.setChildCount(argResults.size())
                .maybeReverse()
                .fieldName("arguments", ExplainVersion::V3)
                .print(argResults);
        }
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const Source& expr) {
        ExplainPrinter printer("Source");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter generate(const ABT::reference_type node) {
        return algebra::transport<true>(node, *this);
    }
};

using ExplainGeneratorV2 = ExplainGeneratorTransporter<ExplainVersion::V2>;
using ExplainGeneratorV3 = ExplainGeneratorTransporter<ExplainVersion::V3>;

std::string ExplainGenerator::explainV2(const ABT::reference_type node) {
    ExplainGeneratorV2 gen;
    return gen.generate(node).str();
}

std::pair<sbe::value::TypeTags, sbe::value::Value> ExplainGenerator::explainBSON(
    const ABT::reference_type node) {
    ExplainGeneratorV3 gen;
    return gen.generate(node).moveValue();
}

template <class PrinterType>
static void printBSONstr(PrinterType& printer,
                         const sbe::value::TypeTags tag,
                         const sbe::value::Value val) {
    switch (tag) {
        case sbe::value::TypeTags::Array: {
            const auto* array = sbe::value::getArrayView(val);

            PrinterType local;
            for (size_t index = 0; index < array->size(); index++) {
                if (index > 0) {
                    local.print(", ");
                    local.newLine();
                }
                const auto [tag1, val1] = array->getAt(index);
                printBSONstr(local, tag1, val1);
            }
            printer.print("[").print(local).print("]");

            break;
        }

        case sbe::value::TypeTags::Object: {
            const auto* obj = sbe::value::getObjectView(val);

            PrinterType local;
            for (size_t index = 0; index < obj->size(); index++) {
                if (index > 0) {
                    local.print(", ");
                    local.newLine();
                }
                local.fieldName(obj->field(index));
                const auto [tag1, val1] = obj->getAt(index);
                printBSONstr(local, tag1, val1);
            }
            printer.print("{").print(local).print("}");

            break;
        }

        default: {
            std::ostringstream os;
            os << std::make_pair(tag, val);
            printer.print(os.str());
        }
    }
}

std::string ExplainGenerator::explainBSONStr(const ABT::reference_type node) {
    const auto [tag, val] = explainBSON(node);
    sbe::value::ValueGuard vg(tag, val);
    ExplainPrinterImpl<ExplainVersion::V2> printer;
    printBSONstr(printer, tag, val);
    return printer.str();
}
}  // namespace mongo::abt
