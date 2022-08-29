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

#include "mongo/db/query/optimizer/explain.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

BSONObj ABTPrinter::explainBSON() const {
    return ExplainGenerator::explainBSONObj(
        _abtTree, true /*displayProperties*/, nullptr /*Memo*/, _nodeToPropsMap);
}

enum class ExplainVersion { V1, V2, V3, Vmax };

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

static constexpr ExplainVersion kDefaultExplainVersion = ExplainVersion::V1;

enum class CommandType { Indent, Unindent, AddLine };

struct CommandStruct {
    CommandStruct() = default;
    CommandStruct(const CommandType type, std::string str) : _type(type), _str(std::move(str)) {}

    CommandType _type;
    std::string _str;
};

using CommandVector = std::vector<CommandStruct>;

template <const ExplainVersion version = kDefaultExplainVersion>
class ExplainPrinterImpl {
public:
    ExplainPrinterImpl()
        : _cmd(),
          _os(),
          _osDirty(false),
          _indentCount(0),
          _childrenRemaining(0),
          _cmdInsertPos(-1) {}

    ~ExplainPrinterImpl() {
        uassert(6624003, "Unmatched indentations", _indentCount == 0);
        uassert(6624004, "Incorrect child count mark", _childrenRemaining == 0);
    }

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
          _cmdInsertPos(other._cmdInsertPos) {}

    template <class T>
    ExplainPrinterImpl& print(const T& t) {
        _os << t;
        _osDirty = true;
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

    ExplainPrinterImpl& setChildCount(const size_t childCount) {
        if (version > ExplainVersion::V1) {
            _childrenRemaining = childCount;
            indent("");
            for (int i = 0; i < _childrenRemaining - 1; i++) {
                indent("|");
            }
        }
        return *this;
    }

    ExplainPrinterImpl& maybeReverse() {
        if (version > ExplainVersion::V1) {
            _cmdInsertPos = _cmd.size();
        }
        return *this;
    }

    ExplainPrinterImpl& fieldName(const std::string& name,
                                  const ExplainVersion minVersion = ExplainVersion::V1,
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
                            os << element << ((version == ExplainVersion::V1) ? " " : "   ");
                        }
                    }
                    os << cmd._str << "\n";
                    break;
                }

                default: { MONGO_UNREACHABLE; }
            }
        }

        return os.str();
    }

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
        } else {
            newLine();
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

    CommandVector _cmd;
    std::ostringstream _os;
    bool _osDirty;
    int _indentCount;
    int _childrenRemaining;
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
        auto [tag, val] = sbe::value::copyValue(v.first, v.second);
        addValue(tag, val);
        return *this;
    }

    ExplainPrinterImpl& print(const std::string& s) {
        auto [tag, val] = sbe::value::makeNewString(s);
        addValue(tag, val);
        return *this;
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

    ExplainPrinterImpl& fieldName(const std::string& name,
                                  const ExplainVersion minVersion = ExplainVersion::V1,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        if (minVersion <= version && maxVersion >= version) {
            _nextFieldName = name;
        }
        return *this;
    }

    ExplainPrinterImpl& separator(const std::string& /*separator*/) {
        // Ignored.
        return *this;
    }

private:
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
            _canAppend = !_nextFieldName.empty();
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
            uassert(6624073, "Field name is not empty", _nextFieldName.empty());
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
            addField(_nextFieldName, tag, val);
            _nextFieldName.clear();
        }
    }

    void addField(const std::string& fieldName, sbe::value::TypeTags tag, sbe::value::Value val) {
        uassert(6624074, "Field name is empty", !fieldName.empty());
        uassert(6624075, "Duplicate field name", _fieldNameSet.insert(fieldName).second);
        sbe::value::getObjectView(_val)->push_back(fieldName, tag, val);
    }

    void reset() {
        _nextFieldName.clear();
        _initialized = false;
        _canAppend = false;
        _tag = sbe::value::TypeTags::Nothing;
        _val = 0;
        _fieldNameSet.clear();
    }

    std::string _nextFieldName;
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

    ExplainGeneratorTransporter(bool displayProperties = false,
                                const cascades::Memo* memo = nullptr,
                                const NodeToGroupPropsMap& nodeMap = {})
        : _displayProperties(displayProperties), _memo(memo), _nodeMap(nodeMap) {
        uassert(6624005,
                "Memo must be provided in order to display properties.",
                !_displayProperties || (_memo != nullptr || version == ExplainVersion::V3));
    }

    /**
     * Helper function that appends the logical and physical properties of 'node' nested under a new
     * field named 'properties'. Only applicable for BSON explain, for other versions this is a
     * no-op.
     */
    void maybePrintProps(ExplainPrinter& nodePrinter, const Node& node) {
        if (!_displayProperties || version != ExplainVersion::V3 || _nodeMap.empty()) {
            return;
        }
        auto it = _nodeMap.find(&node);
        uassert(6624006, "Failed to find node properties", it != _nodeMap.end());

        const NodeProps& props = it->second;

        ExplainPrinter logPropPrinter = printLogicalProps("logical", props._logicalProps);
        ExplainPrinter physPropPrinter = printPhysProps("physical", props._physicalProps);

        ExplainPrinter propsPrinter;
        propsPrinter.fieldName("cost")
            .print(props._cost.getCost())
            .fieldName("localCost")
            .print(props._localCost.getCost())
            .fieldName("adjustedCE")
            .print(props._adjustedCE)
            .fieldName("planNodeID")
            .print(props._planNodeId)
            .fieldName("logicalProperties")
            .print(logPropPrinter)
            .fieldName("physicalProperties")
            .print(physPropPrinter);
        ExplainPrinter res;
        res.fieldName("properties").print(propsPrinter);
        nodePrinter.printAppend(res);
    }

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

    /**
     * Nodes
     */
    ExplainPrinter transport(const References& references, std::vector<ExplainPrinter> inResults) {
        ExplainPrinter printer;
        printer.separator("RefBlock: ").printAppend(inResults);
        return printer;
    }

    ExplainPrinter transport(const ExpressionBinder& binders,
                             std::vector<ExplainPrinter> inResults) {
        std::map<std::string, ExplainPrinter> ordered;
        for (size_t idx = 0; idx < inResults.size(); ++idx) {
            ordered.emplace(binders.names()[idx], std::move(inResults[idx]));
        }

        ExplainPrinter printer;
        printer.separator("BindBlock:");

        for (auto& [name, child] : ordered) {
            if constexpr (version < ExplainVersion::V3) {
                ExplainPrinter local;
                local.print("[").print(name).print("]").print(child);
                printer.print(local);
            } else if constexpr (version == ExplainVersion::V3) {
                printer.separator(" ").fieldName(name).print(child);
            } else {
                MONGO_UNREACHABLE;
            }
        }

        return printer;
    }

    static void printFieldProjectionMap(ExplainPrinter& printer, const FieldProjectionMap& map) {
        std::map<FieldNameType, ProjectionName> ordered;
        if (!map._ridProjection.empty()) {
            ordered["<rid>"] = map._ridProjection;
        }
        if (!map._rootProjection.empty()) {
            ordered["<root>"] = map._rootProjection;
        }
        for (const auto& entry : map._fieldProjections) {
            ordered.insert(entry);
        }

        if constexpr (version < ExplainVersion::V3) {
            bool first = true;
            for (const auto& [fieldName, projectionName] : ordered) {
                if (first) {
                    first = false;
                } else {
                    printer.print(", ");
                }
                printer.print("'").print(fieldName).print("': ").print(projectionName);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            ExplainPrinter local;
            for (const auto& [fieldName, projectionName] : ordered) {
                local.fieldName(fieldName).print(projectionName);
            }
            printer.fieldName("fieldProjectionMap").print(local);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    ExplainPrinter transport(const ScanNode& node, ExplainPrinter bindResult) {
        ExplainPrinter printer("Scan");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("scanDefName", ExplainVersion::V3)
            .print(node.getScanDefName())
            .separator("]")
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const PhysicalScanNode& node, ExplainPrinter bindResult) {
        ExplainPrinter printer("PhysicalScan");
        maybePrintProps(printer, node);

        printer.separator(" [{");
        printFieldProjectionMap(printer, node.getFieldProjectionMap());
        printer.separator("}, ")
            .fieldName("scanDefName", ExplainVersion::V3)
            .print(node.getScanDefName());

        printBooleanFlag(printer, "parallel", node.useParallelScan());

        printer.separator("]").fieldName("bindings", ExplainVersion::V3).print(bindResult);

        return printer;
    }

    ExplainPrinter transport(const ValueScanNode& node, ExplainPrinter bindResult) {
        ExplainPrinter valuePrinter = generate(node.getValueArray());

        ExplainPrinter printer("ValueScan");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("arraySize")
            .print(node.getArraySize())
            .separator("]")
            .fieldName("values", ExplainVersion::V3)
            .print(valuePrinter)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const CoScanNode& node) {
        ExplainPrinter printer("CoScan");
        maybePrintProps(printer, node);
        printer.separator(" []");
        return printer;
    }

    void printInterval(ExplainPrinter& printer, const IntervalRequirement& interval) {
        const BoundRequirement& lowBound = interval.getLowBound();
        const BoundRequirement& highBound = interval.getHighBound();

        if constexpr (version < ExplainVersion::V3) {
            const auto printBoundFn = [](ExplainPrinter& printer, const ABT& bound) {
                // Since we are printing on a single level, use V1 printer in order to avoid
                // children being reversed.
                ExplainGeneratorTransporter<ExplainVersion::V1> gen;
                auto boundPrinter = gen.generate(bound);
                printer.printSingleLevel(boundPrinter);
            };

            printer.print(lowBound.isInclusive() ? "[" : "(");
            printBoundFn(printer, lowBound.getBound());

            printer.print(", ");
            printBoundFn(printer, highBound.getBound());

            printer.print(highBound.isInclusive() ? "]" : ")");
        } else if constexpr (version == ExplainVersion::V3) {
            ExplainPrinter lowBoundPrinter;
            lowBoundPrinter.fieldName("inclusive").print(lowBound.isInclusive());
            {
                ExplainPrinter boundPrinter = generate(lowBound.getBound());
                lowBoundPrinter.fieldName("bound").print(boundPrinter);
            }

            ExplainPrinter highBoundPrinter;
            highBoundPrinter.fieldName("inclusive").print(highBound.isInclusive());
            {
                ExplainPrinter boundPrinter = generate(highBound.getBound());
                highBoundPrinter.fieldName("bound").print(boundPrinter);
            }

            printer.fieldName("lowBound")
                .print(lowBoundPrinter)
                .fieldName("highBound")
                .print(highBoundPrinter);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    std::string printInterval(const IntervalRequirement& interval) {
        ExplainPrinter printer;
        printInterval(printer, interval);
        return printer.str();
    }

    ExplainPrinter printIntervalExpr(const IntervalReqExpr::Node& intervalExpr) {
        IntervalPrinter<IntervalReqExpr> intervalPrinter(*this);
        return intervalPrinter.print(intervalExpr);
    }

    void printInterval(ExplainPrinter& printer, const CompoundIntervalRequirement& interval) {
        if constexpr (version < ExplainVersion::V3) {
            bool first = true;
            for (const auto& entry : interval) {
                if (first) {
                    first = false;
                } else {
                    printer.print(", ");
                }
                printInterval(printer, entry);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const auto& entry : interval) {
                ExplainPrinter local;
                printInterval(local, entry);
                printers.push_back(std::move(local));
            }
            printer.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    template <class T>
    class IntervalPrinter {
    public:
        IntervalPrinter(ExplainGeneratorTransporter& instance) : _instance(instance) {}

        ExplainPrinter transport(const typename T::Atom& node) {
            ExplainPrinter printer;
            printer.separator("{");
            _instance.printInterval(printer, node.getExpr());
            printer.separator("}");
            return printer;
        }

        template <bool isConjunction>
        ExplainPrinter print(std::vector<ExplainPrinter> childResults) {
            if constexpr (version < ExplainVersion::V3) {
                ExplainPrinter printer;
                printer.separator("{");

                bool first = true;
                for (ExplainPrinter& child : childResults) {
                    if (first) {
                        first = false;
                    } else if constexpr (isConjunction) {
                        printer.print(" ^ ");
                    } else {
                        printer.print(" U ");
                    }
                    printer.print(child);
                }
                printer.separator("}");

                return printer;
            } else if constexpr (version == ExplainVersion::V3) {
                ExplainPrinter printer;
                if constexpr (isConjunction) {
                    printer.fieldName("conjunction");
                } else {
                    printer.fieldName("disjunction");
                }
                printer.print(childResults);
                return printer;
            } else {
                MONGO_UNREACHABLE;
            }
        }

        ExplainPrinter transport(const typename T::Conjunction& node,
                                 std::vector<ExplainPrinter> childResults) {
            return print<true /*isConjunction*/>(std::move(childResults));
        }

        ExplainPrinter transport(const typename T::Disjunction& node,
                                 std::vector<ExplainPrinter> childResults) {
            return print<false /*isConjunction*/>(std::move(childResults));
        }

        ExplainPrinter print(const typename T::Node& intervals) {
            return algebra::transport<false>(intervals, *this);
        }

    private:
        ExplainGeneratorTransporter& _instance;
    };

    ExplainPrinter transport(const IndexScanNode& node, ExplainPrinter bindResult) {
        ExplainPrinter printer("IndexScan");
        maybePrintProps(printer, node);

        printer.separator(" [{");
        printFieldProjectionMap(printer, node.getFieldProjectionMap());
        printer.separator("}, ");

        const auto& spec = node.getIndexSpecification();
        printer.fieldName("scanDefName")
            .print(spec.getScanDefName())
            .separator(", ")
            .fieldName("indexDefName")
            .print(spec.getIndexDefName())
            .separator(", ");

        printer.fieldName("interval").separator("{");
        printInterval(printer, spec.getInterval());
        printer.separator("}");

        printBooleanFlag(printer, "reversed", spec.isReverseOrder());

        printer.separator("]").fieldName("bindings", ExplainVersion::V3).print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const SeekNode& node,
                             ExplainPrinter bindResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Seek");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("ridProjection")
            .print(node.getRIDProjectionName())
            .separator(", {");
        printFieldProjectionMap(printer, node.getFieldProjectionMap());
        printer.separator("}, ")
            .fieldName("scanDefName", ExplainVersion::V3)
            .print(node.getScanDefName())
            .separator("]")
            .setChildCount(2)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("references", ExplainVersion::V3)
            .print(refsResult);

        return printer;
    }

    ExplainPrinter transport(const MemoLogicalDelegatorNode& node) {
        ExplainPrinter printer("MemoLogicalDelegator");
        maybePrintProps(printer, node);
        printer.separator(" [").fieldName("groupId").print(node.getGroupId()).separator("]");
        return printer;
    }

    ExplainPrinter transport(const MemoPhysicalDelegatorNode& node) {
        const auto id = node.getNodeId();

        if (_displayProperties) {
            const auto& group = _memo->getGroup(id._groupId);
            const auto& result = group._physicalNodes.at(id._index);
            uassert(6624076,
                    "Physical delegator must be pointing to an optimized result.",
                    result._nodeInfo.has_value());

            const auto& nodeInfo = *result._nodeInfo;
            const ABT& n = nodeInfo._node;

            ExplainPrinter nodePrinter = generate(n);
            if (n.template is<MemoPhysicalDelegatorNode>()) {
                // Handle delegation.
                return nodePrinter;
            }

            ExplainPrinter logPropPrinter = printLogicalProps("Logical", group._logicalProperties);
            ExplainPrinter physPropPrinter = printPhysProps("Physical", result._physProps);

            ExplainPrinter printer("Properties");
            printer.separator(" [")
                .fieldName("cost")
                .print(nodeInfo._cost.getCost())
                .separator(", ")
                .fieldName("localCost")
                .print(nodeInfo._localCost.getCost())
                .separator(", ")
                .fieldName("adjustedCE")
                .print(nodeInfo._adjustedCE)
                .separator("]")
                .setChildCount(3)
                .fieldName("logicalProperties", ExplainVersion::V3)
                .print(logPropPrinter)
                .fieldName("physicalProperties", ExplainVersion::V3)
                .print(physPropPrinter)
                .fieldName("node", ExplainVersion::V3)
                .print(nodePrinter);
            return printer;
        }

        ExplainPrinter printer("MemoPhysicalDelegator");
        printer.separator(" [")
            .fieldName("groupId")
            .print(id._groupId)
            .separator(", ")
            .fieldName("index")
            .print(id._index)
            .separator("]");
        return printer;
    }

    ExplainPrinter transport(const FilterNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter filterResult) {
        ExplainPrinter printer("Filter");
        maybePrintProps(printer, node);
        printer.separator(" []")
            .setChildCount(2)
            .fieldName("filter", ExplainVersion::V3)
            .print(filterResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    ExplainPrinter transport(const EvaluationNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter projectionResult) {
        ExplainPrinter printer("Evaluation");
        maybePrintProps(printer, node);
        printer.separator(" []")
            .setChildCount(2)
            .fieldName("projection", ExplainVersion::V3)
            .print(projectionResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    void printPartialSchemaReqMap(ExplainPrinter& parent, const PartialSchemaRequirements& reqMap) {
        std::vector<ExplainPrinter> printers;
        for (const auto& [key, req] : reqMap) {
            ExplainPrinter local;

            local.fieldName("refProjection").print(key._projectionName).separator(", ");
            ExplainPrinter pathPrinter = generate(key._path);
            local.fieldName("path").separator("'").printSingleLevel(pathPrinter).separator("', ");

            if (req.hasBoundProjectionName()) {
                local.fieldName("boundProjection")
                    .print(req.getBoundProjectionName())
                    .separator(", ");
            }

            local.fieldName("intervals");
            {
                ExplainPrinter intervals = printIntervalExpr(req.getIntervals());
                local.printSingleLevel(intervals, "" /*singleLevelSpacer*/);
            }

            printers.push_back(std::move(local));
        }

        parent.fieldName("requirementsMap").print(printers);
    }

    void printResidualRequirements(ExplainPrinter& parent,
                                   const ResidualRequirements& residualReqs) {
        std::vector<ExplainPrinter> printers;
        for (const auto& [key, req, entryIndex] : residualReqs) {
            ExplainPrinter local;

            local.fieldName("refProjection").print(key._projectionName).separator(", ");
            ExplainPrinter pathPrinter = generate(key._path);
            local.fieldName("path").separator("'").printSingleLevel(pathPrinter).separator("', ");

            if (req.hasBoundProjectionName()) {
                local.fieldName("boundProjection")
                    .print(req.getBoundProjectionName())
                    .separator(", ");
            }

            local.fieldName("intervals");
            {
                ExplainPrinter intervals = printIntervalExpr(req.getIntervals());
                local.printSingleLevel(intervals, "" /*singleLevelSpacer*/);
            }
            local.separator(", ").fieldName("entryIndex").print(entryIndex);

            printers.push_back(std::move(local));
        }

        parent.fieldName("residualReqs").print(printers);
    }

    ExplainPrinter transport(const SargableNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter bindResult,
                             ExplainPrinter refsResult) {
        const auto& scanParams = node.getScanParams();

        ExplainPrinter printer("Sargable");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("target", ExplainVersion::V3)
            .print(IndexReqTargetEnum::toString[static_cast<int>(node.getTarget())])
            .separator("]")
            .setChildCount(scanParams ? 6 : 5);

        if constexpr (version < ExplainVersion::V3) {
            ExplainPrinter local;
            printPartialSchemaReqMap(local, node.getReqMap());
            printer.print(local);
        } else if constexpr (version == ExplainVersion::V3) {
            printPartialSchemaReqMap(printer, node.getReqMap());
        } else {
            MONGO_UNREACHABLE;
        }

        {
            std::vector<ExplainPrinter> candidateIndexesPrinters;
            for (size_t index = 0; index < node.getCandidateIndexes().size(); index++) {
                const CandidateIndexEntry& candidateIndexEntry =
                    node.getCandidateIndexes().at(index);

                ExplainPrinter local;
                local.fieldName("candidateId")
                    .print(index + 1)
                    .separator(", ")
                    .fieldName("indexDefName", ExplainVersion::V3)
                    .print(candidateIndexEntry._indexDefName)
                    .separator(", ");

                local.separator("{");
                printFieldProjectionMap(local, candidateIndexEntry._fieldProjectionMap);
                local.separator("}, {");

                {
                    std::set<size_t> orderedFields;
                    for (const size_t fieldId : candidateIndexEntry._fieldsToCollate) {
                        orderedFields.insert(fieldId);
                    }

                    if constexpr (version < ExplainVersion::V3) {
                        bool first = true;
                        for (const size_t fieldId : orderedFields) {
                            if (first) {
                                first = false;
                            } else {
                                local.print(", ");
                            }
                            local.print(fieldId);
                        }
                    } else if constexpr (version == ExplainVersion::V3) {
                        std::vector<ExplainPrinter> printers;
                        for (const size_t fieldId : orderedFields) {
                            ExplainPrinter local1;
                            local1.print(fieldId);
                            printers.push_back(std::move(local1));
                        }
                        local.fieldName("fieldsToCollate").print(printers);
                    } else {
                        MONGO_UNREACHABLE;
                    }
                }

                local.separator("}, ").fieldName("intervals", ExplainVersion::V3);
                {
                    IntervalPrinter<CompoundIntervalReqExpr> intervalPrinter(*this);
                    ExplainPrinter intervals =
                        intervalPrinter.print(candidateIndexEntry._intervals);
                    local.printSingleLevel(intervals, "" /*singleLevelSpacer*/);
                }

                if (const auto& residualReqs = candidateIndexEntry._residualRequirements;
                    !residualReqs.empty()) {
                    if constexpr (version < ExplainVersion::V3) {
                        ExplainPrinter residualReqMapPrinter;
                        printResidualRequirements(residualReqMapPrinter, residualReqs);
                        local.print(residualReqMapPrinter);
                    } else if (version == ExplainVersion::V3) {
                        printResidualRequirements(local, residualReqs);
                    } else {
                        MONGO_UNREACHABLE;
                    }
                }

                candidateIndexesPrinters.push_back(std::move(local));
            }
            ExplainPrinter candidateIndexesPrinter;
            candidateIndexesPrinter.fieldName("candidateIndexes").print(candidateIndexesPrinters);
            printer.printAppend(candidateIndexesPrinter);
        }

        if (scanParams) {
            ExplainPrinter local;
            local.separator("{");
            printFieldProjectionMap(local, scanParams->_fieldProjectionMap);
            local.separator("}");

            if (const auto& residualReqs = scanParams->_residualRequirements;
                !residualReqs.empty()) {
                if constexpr (version < ExplainVersion::V3) {
                    ExplainPrinter residualReqMapPrinter;
                    printResidualRequirements(residualReqMapPrinter, residualReqs);
                    local.print(residualReqMapPrinter);
                } else if (version == ExplainVersion::V3) {
                    printResidualRequirements(local, residualReqs);
                } else {
                    MONGO_UNREACHABLE;
                }
            }

            ExplainPrinter scanParamsPrinter;
            scanParamsPrinter.fieldName("scanParams").print(local);
            printer.printAppend(scanParamsPrinter);
        }

        printer.fieldName("bindings", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("references", ExplainVersion::V3)
            .print(refsResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    ExplainPrinter transport(const RIDIntersectNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult) {
        ExplainPrinter printer("RIDIntersect");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("scanProjectionName", ExplainVersion::V3)
            .print(node.getScanProjectionName());
        printBooleanFlag(printer, "hasLeftIntervals", node.hasLeftIntervals());
        printBooleanFlag(printer, "hasRightIntervals", node.hasRightIntervals());

        printer.separator("]")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const BinaryJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter filterResult) {
        ExplainPrinter printer("BinaryJoin");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("joinType")
            .print(JoinTypeEnum::toString[static_cast<int>(node.getJoinType())]);

        ProjectionNameOrderedSet ordered;
        for (const ProjectionName& projName : node.getCorrelatedProjectionNames()) {
            ordered.insert(projName);
        }

        if constexpr (version < ExplainVersion::V3) {
            if (!node.getCorrelatedProjectionNames().empty()) {
                printer.print(", {");
                bool first = true;
                for (const ProjectionName& projectionName : ordered) {
                    if (first) {
                        first = false;
                    } else {
                        printer.print(", ");
                    }
                    printer.print(projectionName);
                }
                printer.print("}");
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const ProjectionName& projectionName : ordered) {
                ExplainPrinter local;
                local.print(projectionName);
                printers.push_back(std::move(local));
            }
            printer.fieldName("correlatedProjections").print(printers);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.separator("]")
            .setChildCount(3)
            .fieldName("expression", ExplainVersion::V3)
            .print(filterResult)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    void printEqualityJoinCondition(ExplainPrinter& printer,
                                    const ProjectionNameVector& leftKeys,
                                    const ProjectionNameVector& rightKeys) {
        if constexpr (version < ExplainVersion::V3) {
            printer.print("Condition");
            for (size_t i = 0; i < leftKeys.size(); i++) {
                ExplainPrinter local;
                local.print(leftKeys.at(i)).print(" = ").print(rightKeys.at(i));
                printer.print(local);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (size_t i = 0; i < leftKeys.size(); i++) {
                ExplainPrinter local;
                local.fieldName("leftKey")
                    .print(leftKeys.at(i))
                    .fieldName("rightKey")
                    .print(rightKeys.at(i));
                printers.push_back(std::move(local));
            }
            printer.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    ExplainPrinter transport(const HashJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("HashJoin");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("joinType")
            .print(JoinTypeEnum::toString[static_cast<int>(node.getJoinType())])
            .separator("]");

        ExplainPrinter joinConditionPrinter;
        printEqualityJoinCondition(joinConditionPrinter, node.getLeftKeys(), node.getRightKeys());

        printer.setChildCount(3)
            .fieldName("joinCondition", ExplainVersion::V3)
            .print(joinConditionPrinter)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const MergeJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("MergeJoin");
        maybePrintProps(printer, node);
        printer.separator(" []");

        ExplainPrinter joinConditionPrinter;
        printEqualityJoinCondition(joinConditionPrinter, node.getLeftKeys(), node.getRightKeys());

        ExplainPrinter collationPrinter;
        if constexpr (version < ExplainVersion::V3) {
            collationPrinter.print("Collation");
            for (const CollationOp op : node.getCollation()) {
                ExplainPrinter local;
                local.print(CollationOpEnum::toString[static_cast<int>(op)]);
                collationPrinter.print(local);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const CollationOp op : node.getCollation()) {
                ExplainPrinter local;
                local.print(CollationOpEnum::toString[static_cast<int>(op)]);
                printers.push_back(std::move(local));
            }
            collationPrinter.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.setChildCount(4)
            .fieldName("joinCondition", ExplainVersion::V3)
            .print(joinConditionPrinter)
            .fieldName("collation", ExplainVersion::V3)
            .print(collationPrinter)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const UnionNode& node,
                             std::vector<ExplainPrinter> childResults,
                             ExplainPrinter bindResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("Union");
        maybePrintProps(printer, node);
        printer.separator(" []")
            .setChildCount(childResults.size() + 1)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult)
            .maybeReverse()
            .fieldName("children", ExplainVersion::V3)
            .print(childResults);
        return printer;
    }

    ExplainPrinter transport(const GroupByNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter bindAggResult,
                             ExplainPrinter refsAggResult,
                             ExplainPrinter bindGbResult,
                             ExplainPrinter refsGbResult) {
        std::map<ProjectionName, size_t> ordered;
        const ProjectionNameVector& aggProjectionNames = node.getAggregationProjectionNames();
        for (size_t i = 0; i < aggProjectionNames.size(); i++) {
            ordered.emplace(aggProjectionNames.at(i), i);
        }

        ExplainPrinter printer("GroupBy");
        maybePrintProps(printer, node);
        printer.separator(" [");
        if (version >= ExplainVersion::V3 || node.getType() != GroupNodeType::Complete) {
            printer.fieldName("type", ExplainVersion::V3)
                .print(GroupNodeTypeEnum::toString[static_cast<int>(node.getType())]);
        }
        printer.separator("]");

        std::vector<ExplainPrinter> aggPrinters;
        for (const auto& [projectionName, index] : ordered) {
            ExplainPrinter local;
            local.separator("[")
                .fieldName("projectionName", ExplainVersion::V3)
                .print(projectionName)
                .separator("]");
            ExplainPrinter aggExpr = generate(node.getAggregationExpressions().at(index));
            local.fieldName("aggregation", ExplainVersion::V3).print(aggExpr);
            aggPrinters.push_back(std::move(local));
        }

        ExplainPrinter gbPrinter;
        gbPrinter.fieldName("groupings").print(refsGbResult);

        ExplainPrinter aggPrinter;
        aggPrinter.fieldName("aggregations").print(aggPrinters);

        printer.setChildCount(3)
            .printAppend(gbPrinter)
            .printAppend(aggPrinter)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    ExplainPrinter transport(const UnwindNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter bindResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Unwind");
        maybePrintProps(printer, node);

        printer.separator(" [");
        printBooleanFlag(printer, "retainNonArrays", node.getRetainNonArrays(), false /*addComma*/);
        printer.separator("]");

        printer.setChildCount(2)
            .fieldName("bind", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    static void printCollationProperty(ExplainPrinter& parent,
                                       const properties::CollationRequirement& property,
                                       const bool directToParent) {
        std::vector<ExplainPrinter> propPrinters;
        for (const auto& entry : property.getCollationSpec()) {
            ExplainPrinter local;
            local.fieldName("projectionName", ExplainVersion::V3)
                .print(entry.first)
                .separator(": ")
                .fieldName("collationOp", ExplainVersion::V3)
                .print(CollationOpEnum::toString[static_cast<int>(entry.second)]);
            propPrinters.push_back(std::move(local));
        }

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printer.fieldName("collation").print(propPrinters);
        });
    }

    ExplainPrinter transport(const UniqueNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("Unique");
        maybePrintProps(printer, node);

        printer.separator(" []").setChildCount(2);
        printPropertyProjections(printer, node.getProjections(), false /*directToParent*/);
        printer.fieldName("child", ExplainVersion::V3).print(childResult);

        return printer;
    }

    ExplainPrinter transport(const CollationNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Collation");
        maybePrintProps(printer, node);

        printer.separator(" []").setChildCount(3);
        printCollationProperty(printer, node.getProperty(), false /*directToParent*/);
        printer.fieldName("references", ExplainVersion::V3)
            .print(refsResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);

        return printer;
    }

    static void printLimitSkipProperty(ExplainPrinter& propPrinter,
                                       ExplainPrinter& limitPrinter,
                                       ExplainPrinter& skipPrinter,
                                       const properties::LimitSkipRequirement& property) {
        propPrinter.fieldName("propType", ExplainVersion::V3)
            .print("limitSkip")
            .separator(":")
            .printAppend(limitPrinter)
            .printAppend(skipPrinter);
    }

    static void printLimitSkipProperty(ExplainPrinter& parent,
                                       const properties::LimitSkipRequirement& property,
                                       const bool directToParent) {
        ExplainPrinter limitPrinter;
        limitPrinter.fieldName("limit");
        if (property.hasLimit()) {
            limitPrinter.print(property.getLimit());
        } else {
            limitPrinter.print("(none)");
        }

        ExplainPrinter skipPrinter;
        skipPrinter.fieldName("skip").print(property.getSkip());

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printLimitSkipProperty(printer, limitPrinter, skipPrinter, property);
        });
    }

    ExplainPrinter transport(const LimitSkipNode& node, ExplainPrinter childResult) {
        ExplainPrinter printer("LimitSkip");
        maybePrintProps(printer, node);

        printer.separator(" []").setChildCount(2);
        printLimitSkipProperty(printer, node.getProperty(), false /*directToParent*/);
        printer.fieldName("child", ExplainVersion::V3).print(childResult);

        return printer;
    }

    static void printPropertyProjections(ExplainPrinter& parent,
                                         const ProjectionNameVector& projections,
                                         const bool directToParent) {
        std::vector<ExplainPrinter> printers;
        for (const ProjectionName& projection : projections) {
            ExplainPrinter local;
            local.print(projection);
            printers.push_back(std::move(local));
        }

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printer.fieldName("projections");
            if (printers.empty()) {
                ExplainPrinter dummy;
                printer.print(dummy);
            } else {
                printer.print(printers);
            }
        });
    }

    static void printDistributionProperty(ExplainPrinter& parent,
                                          const properties::DistributionRequirement& property,
                                          const bool directToParent) {
        const auto& distribAndProjections = property.getDistributionAndProjections();

        ExplainPrinter typePrinter;
        typePrinter.fieldName("type").print(
            DistributionTypeEnum::toString[static_cast<int>(distribAndProjections._type)]);

        printBooleanFlag(typePrinter, "disableExchanges", property.getDisableExchanges());

        const bool hasProjections = !distribAndProjections._projectionNames.empty();
        ExplainPrinter projectionPrinter;
        if (hasProjections) {
            printPropertyProjections(
                projectionPrinter, distribAndProjections._projectionNames, true /*directToParent*/);
            typePrinter.printAppend(projectionPrinter);
        }

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printer.fieldName("distribution").print(typePrinter);
        });
    }

    static void printProjectionRequirementProperty(
        ExplainPrinter& parent,
        const properties::ProjectionRequirement& property,
        const bool directToParent) {
        printPropertyProjections(parent, property.getProjections().getVector(), directToParent);
    }

    ExplainPrinter transport(const ExchangeNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Exchange");
        maybePrintProps(printer, node);

        printer.separator(" []").setChildCount(3);
        printDistributionProperty(printer, node.getProperty(), false /*directToParent*/);
        printer.fieldName("references", ExplainVersion::V3)
            .print(refsResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);

        return printer;
    }

    struct LogicalPropPrintVisitor {
        LogicalPropPrintVisitor(ExplainPrinter& parent) : _parent(parent){};

        void operator()(const properties::LogicalProperty&,
                        const properties::ProjectionAvailability& prop) {
            ProjectionNameOrderedSet ordered;
            for (const ProjectionName& projection : prop.getProjections()) {
                ordered.insert(projection);
            }

            std::vector<ExplainPrinter> printers;
            for (const ProjectionName& projection : ordered) {
                ExplainPrinter local;
                local.print(projection);
                printers.push_back(std::move(local));
            }
            _parent.fieldName("projections").print(printers);
        }

        void operator()(const properties::LogicalProperty&,
                        const properties::CardinalityEstimate& prop) {
            std::vector<ExplainPrinter> fieldPrinters;

            ExplainPrinter cePrinter;
            cePrinter.fieldName("ce").print(prop.getEstimate());
            fieldPrinters.push_back(std::move(cePrinter));

            if (const auto& partialSchemaKeyCE = prop.getPartialSchemaKeyCE();
                !partialSchemaKeyCE.empty()) {
                std::vector<ExplainPrinter> reqPrinters;
                for (const auto& [key, ce] : partialSchemaKeyCE) {
                    ExplainGeneratorTransporter<version> gen;
                    ExplainPrinter pathPrinter = gen.generate(key._path);

                    ExplainPrinter local;
                    local.fieldName("refProjection")
                        .print(key._projectionName)
                        .separator(", ")
                        .fieldName("path")
                        .separator("'")
                        .printSingleLevel(pathPrinter)
                        .separator("', ")
                        .fieldName("ce")
                        .print(ce);
                    reqPrinters.push_back(std::move(local));
                }
                ExplainPrinter requirementsPrinter;
                requirementsPrinter.fieldName("requirementCEs").print(reqPrinters);
                fieldPrinters.push_back(std::move(requirementsPrinter));
            }

            _parent.fieldName("cardinalityEstimate").print(fieldPrinters);
        }

        void operator()(const properties::LogicalProperty&,
                        const properties::IndexingAvailability& prop) {
            ExplainPrinter printer;
            printer.separator("[")
                .fieldName("groupId")
                .print(prop.getScanGroupId())
                .separator(", ")
                .fieldName("scanProjection")
                .print(prop.getScanProjection())
                .separator(", ")
                .fieldName("scanDefName")
                .print(prop.getScanDefName());
            printBooleanFlag(printer, "eqPredsOnly", prop.getEqPredsOnly());
            printer.separator("]");

            if (!prop.getSatisfiedPartialIndexes().empty()) {
                std::set<std::string> ordered;
                for (const auto& indexName : prop.getSatisfiedPartialIndexes()) {
                    ordered.insert(indexName);
                }

                std::vector<ExplainPrinter> printers;
                for (const auto& indexName : ordered) {
                    ExplainPrinter local;
                    local.print(indexName);
                    printers.push_back(std::move(local));
                }
                printer.fieldName("satisfiedPartialIndexes").print(printers);
            }

            _parent.fieldName("indexingAvailability").print(printer);
        }

        void operator()(const properties::LogicalProperty&,
                        const properties::CollectionAvailability& prop) {
            std::set<std::string> orderedSet;
            for (const std::string& scanDef : prop.getScanDefSet()) {
                orderedSet.insert(scanDef);
            }

            std::vector<ExplainPrinter> printers;
            for (const std::string& scanDef : orderedSet) {
                ExplainPrinter local;
                local.print(scanDef);
                printers.push_back(std::move(local));
            }
            if (printers.empty()) {
                ExplainPrinter dummy;
                printers.push_back(std::move(dummy));
            }

            _parent.fieldName("collectionAvailability").print(printers);
        }

        void operator()(const properties::LogicalProperty&,
                        const properties::DistributionAvailability& prop) {
            struct Comparator {
                bool operator()(const properties::DistributionRequirement& d1,
                                const properties::DistributionRequirement& d2) const {
                    const properties::DistributionAndProjections& distr1 =
                        d1.getDistributionAndProjections();
                    const properties::DistributionAndProjections& distr2 =
                        d2.getDistributionAndProjections();

                    if (distr1._type < distr2._type) {
                        return true;
                    }
                    if (distr1._type > distr2._type) {
                        return false;
                    }
                    return distr1._projectionNames < distr2._projectionNames;
                }
            };

            std::set<properties::DistributionRequirement, Comparator> ordered;
            for (const auto& distributionProp : prop.getDistributionSet()) {
                ordered.insert(distributionProp);
            }

            std::vector<ExplainPrinter> printers;
            for (const auto& distributionProp : ordered) {
                ExplainPrinter local;
                printDistributionProperty(local, distributionProp, true /*directToParent*/);
                printers.push_back(std::move(local));
            }
            _parent.fieldName("distributionAvailability").print(printers);
        }

    private:
        // We don't own this.
        ExplainPrinter& _parent;
    };

    struct PhysPropPrintVisitor {
        PhysPropPrintVisitor(ExplainPrinter& parent) : _parent(parent){};

        void operator()(const properties::PhysProperty&,
                        const properties::CollationRequirement& prop) {
            printCollationProperty(_parent, prop, true /*directToParent*/);
        }

        void operator()(const properties::PhysProperty&,
                        const properties::LimitSkipRequirement& prop) {
            printLimitSkipProperty(_parent, prop, true /*directToParent*/);
        }

        void operator()(const properties::PhysProperty&,
                        const properties::ProjectionRequirement& prop) {
            printProjectionRequirementProperty(_parent, prop, true /*directToParent*/);
        }

        void operator()(const properties::PhysProperty&,
                        const properties::DistributionRequirement& prop) {
            printDistributionProperty(_parent, prop, true /*directToParent*/);
        }

        void operator()(const properties::PhysProperty&,
                        const properties::IndexingRequirement& prop) {
            ExplainPrinter printer;

            printer.fieldName("target", ExplainVersion::V3)
                .print(IndexReqTargetEnum::toString[static_cast<int>(prop.getIndexReqTarget())]);
            printBooleanFlag(printer, "dedupRID", prop.getDedupRID());

            // TODO: consider printing satisfied partial indexes.
            _parent.fieldName("indexingRequirement").print(printer);
        }

        void operator()(const properties::PhysProperty&,
                        const properties::RepetitionEstimate& prop) {
            _parent.fieldName("repetitionEstimate").print(prop.getEstimate());
        }

        void operator()(const properties::PhysProperty&, const properties::LimitEstimate& prop) {
            _parent.fieldName("limitEstimate").print(prop.getEstimate());
        }

    private:
        // We don't own this.
        ExplainPrinter& _parent;
    };

    template <class P, class V, class C>
    static ExplainPrinter printProps(const std::string& description, const C& props) {
        ExplainPrinter printer;
        if (version < ExplainVersion::V3) {
            printer.print(description).print(":");
        }

        std::map<typename P::key_type, P> ordered;
        for (const auto& entry : props) {
            ordered.insert(entry);
        }

        ExplainPrinter local;
        V visitor(local);
        for (const auto& entry : ordered) {
            entry.second.visit(visitor);
        }
        printer.print(local);

        return printer;
    }

    static ExplainPrinter printLogicalProps(const std::string& description,
                                            const properties::LogicalProps& props) {
        return printProps<properties::LogicalProperty, LogicalPropPrintVisitor>(description, props);
    }

    static ExplainPrinter printPhysProps(const std::string& description,
                                         const properties::PhysProps& props) {
        return printProps<properties::PhysProperty, PhysPropPrintVisitor>(description, props);
    }

    ExplainPrinter transport(const RootNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Root");
        maybePrintProps(printer, node);

        printer.separator(" []").setChildCount(3);
        printProjectionRequirementProperty(printer, node.getProperty(), false /*directToParent*/);
        printer.fieldName("references", ExplainVersion::V3)
            .print(refsResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);

        return printer;
    }

    /**
     * Expressions
     */
    ExplainPrinter transport(const Blackhole& expr) {
        ExplainPrinter printer("Blackhole");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const Constant& expr) {
        ExplainPrinter printer("Const");
        printer.separator(" [")
            .fieldName("value", ExplainVersion::V3)
            .print(expr.get())
            .separator("]");
        return printer;
    }

    ExplainPrinter transport(const Variable& expr) {
        ExplainPrinter printer("Variable");
        printer.separator(" [")
            .fieldName("name", ExplainVersion::V3)
            .print(expr.name())
            .separator("]");
        return printer;
    }

    ExplainPrinter transport(const UnaryOp& expr, ExplainPrinter inResult) {
        ExplainPrinter printer("UnaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(OperationsEnum::toString[static_cast<int>(expr.op())])
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const BinaryOp& expr,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("BinaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(OperationsEnum::toString[static_cast<int>(expr.op())])
            .separator("]")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("left", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("right", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }


    ExplainPrinter transport(const If& expr,
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

    ExplainPrinter transport(const Let& expr,
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

    ExplainPrinter transport(const LambdaAbstraction& expr, ExplainPrinter inResult) {
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

    ExplainPrinter transport(const LambdaApplication& expr,
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

    ExplainPrinter transport(const FunctionCall& expr, std::vector<ExplainPrinter> argResults) {
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

    ExplainPrinter transport(const EvalPath& expr,
                             ExplainPrinter pathResult,
                             ExplainPrinter inputResult) {
        ExplainPrinter printer("EvalPath");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("path", ExplainVersion::V3)
            .print(pathResult)
            .fieldName("input", ExplainVersion::V3)
            .print(inputResult);
        return printer;
    }

    ExplainPrinter transport(const EvalFilter& expr,
                             ExplainPrinter pathResult,
                             ExplainPrinter inputResult) {
        ExplainPrinter printer("EvalFilter");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("path", ExplainVersion::V3)
            .print(pathResult)
            .fieldName("input", ExplainVersion::V3)
            .print(inputResult);
        return printer;
    }

    /**
     * Paths
     */
    ExplainPrinter transport(const PathConstant& path, ExplainPrinter inResult) {
        ExplainPrinter printer("PathConstant");
        printer.separator(" []")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const PathLambda& path, ExplainPrinter inResult) {
        ExplainPrinter printer("PathLambda");
        printer.separator(" []")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const PathIdentity& path) {
        ExplainPrinter printer("PathIdentity");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const PathDefault& path, ExplainPrinter inResult) {
        ExplainPrinter printer("PathDefault");
        printer.separator(" []")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const PathCompare& path, ExplainPrinter valueResult) {
        ExplainPrinter printer("PathCompare");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(OperationsEnum::toString[static_cast<int>(path.op())])
            .separator("]")
            .setChildCount(1)
            .fieldName("value", ExplainVersion::V3)
            .print(valueResult);
        return printer;
    }

    static void printPathProjections(ExplainPrinter& printer, const std::set<std::string>& names) {
        if constexpr (version < ExplainVersion::V3) {
            bool first = true;
            for (const std::string& s : names) {
                if (first) {
                    first = false;
                } else {
                    printer.print(", ");
                }
                printer.print(s);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const std::string& s : names) {
                ExplainPrinter local;
                local.print(s);
                printers.push_back(std::move(local));
            }
            printer.fieldName("projections").print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    ExplainPrinter transport(const PathDrop& path) {
        ExplainPrinter printer("PathDrop");
        printer.separator(" [");
        printPathProjections(printer, path.getNames());
        printer.separator("]");
        return printer;
    }

    ExplainPrinter transport(const PathKeep& path) {
        ExplainPrinter printer("PathKeep");
        printer.separator(" [");
        printPathProjections(printer, path.getNames());
        printer.separator("]");
        return printer;
    }

    ExplainPrinter transport(const PathObj& path) {
        ExplainPrinter printer("PathObj");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const PathArr& path) {
        ExplainPrinter printer("PathArr");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const PathTraverse& path, ExplainPrinter inResult) {
        ExplainPrinter printer("PathTraverse");
        printer.separator(" [");

        if constexpr (version < ExplainVersion::V3) {
            if (path.getMaxDepth() == PathTraverse::kUnlimited) {
                printer.print("inf");
            } else {
                printer.print(path.getMaxDepth());
            }
        } else if constexpr (version == ExplainVersion::V3) {
            printer.fieldName("maxDepth", ExplainVersion::V3).print(path.getMaxDepth());
        } else {
            MONGO_UNREACHABLE;
        }

        printer.separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const PathField& path, ExplainPrinter inResult) {
        ExplainPrinter printer("PathField");
        printer.separator(" [")
            .fieldName("path", ExplainVersion::V3)
            .print(path.name())
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const PathGet& path, ExplainPrinter inResult) {
        ExplainPrinter printer("PathGet");
        printer.separator(" [")
            .fieldName("path", ExplainVersion::V3)
            .print(path.name())
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const PathComposeM& path,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("PathComposeM");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("leftInput", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("rightInput", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }

    ExplainPrinter transport(const PathComposeA& path,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("PathComposeA");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("leftInput", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("rightInput", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }

    ExplainPrinter transport(const Source& expr) {
        ExplainPrinter printer("Source");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter generate(const ABT& node) {
        return algebra::transport<false>(node, *this);
    }

    void printPhysNodeInfo(ExplainPrinter& printer, const cascades::PhysNodeInfo& nodeInfo) {
        printer.fieldName("cost");
        if (nodeInfo._cost.isInfinite()) {
            printer.print(nodeInfo._cost.toString());
        } else {
            printer.print(nodeInfo._cost.getCost());
        }
        printer.separator(", ")
            .fieldName("localCost")
            .print(nodeInfo._localCost.getCost())
            .separator(", ")
            .fieldName("adjustedCE")
            .print(nodeInfo._adjustedCE);

        ExplainPrinter nodePrinter = generate(nodeInfo._node);
        printer.separator(", ").fieldName("node").print(nodePrinter);
    }

    ExplainPrinter printMemo() {
        std::vector<ExplainPrinter> groupPrinters;
        for (size_t groupId = 0; groupId < _memo->getGroupCount(); groupId++) {
            const cascades::Group& group = _memo->getGroup(groupId);

            ExplainPrinter groupPrinter;
            groupPrinter.fieldName("groupId").print(groupId).setChildCount(3);
            {
                ExplainPrinter logicalPropPrinter =
                    printLogicalProps("Logical properties", group._logicalProperties);
                groupPrinter.fieldName("logicalProperties", ExplainVersion::V3)
                    .print(logicalPropPrinter);
            }

            {
                std::vector<ExplainPrinter> logicalNodePrinters;
                const ABTVector& logicalNodes = group._logicalNodes.getVector();
                for (size_t i = 0; i < logicalNodes.size(); i++) {
                    ExplainPrinter local;
                    local.fieldName("logicalNodeId").print(i).separator(", ");
                    const auto rule = group._rules.at(i);
                    local.fieldName("rule").print(
                        cascades::LogicalRewriterTypeEnum::toString[static_cast<int>(rule)]);

                    ExplainPrinter nodePrinter = generate(logicalNodes.at(i));
                    local.fieldName("node", ExplainVersion::V3).print(nodePrinter);

                    logicalNodePrinters.push_back(std::move(local));
                }
                ExplainPrinter logicalNodePrinter;
                logicalNodePrinter.print(logicalNodePrinters);

                groupPrinter.fieldName("logicalNodes").print(logicalNodePrinter);
            }

            {
                std::vector<ExplainPrinter> physicalNodePrinters;
                for (const auto& physOptResult : group._physicalNodes.getNodes()) {
                    ExplainPrinter local;
                    local.fieldName("physicalNodeId")
                        .print(physOptResult->_index)
                        .separator(", ")
                        .fieldName("costLimit");

                    if (physOptResult->_costLimit.isInfinite()) {
                        local.print(physOptResult->_costLimit.toString());
                    } else {
                        local.print(physOptResult->_costLimit.getCost());
                    }

                    if (physOptResult->_nodeInfo) {
                        const cascades::PhysicalRewriteType rule = physOptResult->_nodeInfo->_rule;
                        local.separator(", ").fieldName("rule").print(
                            cascades::PhysicalRewriterTypeEnum::toString[static_cast<int>(rule)]);
                    }

                    ExplainPrinter propPrinter =
                        printPhysProps("Physical properties", physOptResult->_physProps);
                    local.fieldName("physicalProperties", ExplainVersion::V3).print(propPrinter);

                    if (physOptResult->_nodeInfo) {
                        ExplainPrinter local1;
                        printPhysNodeInfo(local1, *physOptResult->_nodeInfo);

                        if (!physOptResult->_rejectedNodeInfo.empty()) {
                            std::vector<ExplainPrinter> rejectedPrinters;
                            for (const auto& rejectedPlan : physOptResult->_rejectedNodeInfo) {
                                ExplainPrinter local2;
                                printPhysNodeInfo(local2, rejectedPlan);
                                rejectedPrinters.emplace_back(std::move(local2));
                            }
                            local1.fieldName("rejectedPlans").print(rejectedPrinters);
                        }

                        local.fieldName("nodeInfo", ExplainVersion::V3).print(local1);
                    } else {
                        local.separator(" (failed to optimize)");
                    }

                    physicalNodePrinters.push_back(std::move(local));
                }
                ExplainPrinter physNodePrinter;
                physNodePrinter.print(physicalNodePrinters);

                groupPrinter.fieldName("physicalNodes").print(physNodePrinter);
            }

            groupPrinters.push_back(std::move(groupPrinter));
        }

        ExplainPrinter printer;
        printer.fieldName("Memo").print(groupPrinters);
        return printer;
    }

private:
    const bool _displayProperties;

    // We don't own this.
    const cascades::Memo* _memo;
    const NodeToGroupPropsMap& _nodeMap;
};

std::string ExplainGenerator::explain(const ABT& node,
                                      const bool displayProperties,
                                      const cascades::Memo* memo,
                                      const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorTransporter gen(displayProperties, memo, nodeMap);
    return gen.generate(node).str();
}

std::string ExplainGenerator::explainV2(const ABT& node,
                                        const bool displayProperties,
                                        const cascades::Memo* memo,
                                        const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorTransporter<ExplainVersion::V2> gen(displayProperties, memo, nodeMap);
    return gen.generate(node).str();
}

std::string ExplainGenerator::explainNode(const ABT& node) {
    if (node.empty()) {
        return "Empty\n";
    }
    return explainV2(node);
}

std::pair<sbe::value::TypeTags, sbe::value::Value> ExplainGenerator::explainBSON(
    const ABT& node,
    const bool displayProperties,
    const cascades::Memo* memo,
    const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorTransporter<ExplainVersion::V3> gen(displayProperties, memo, nodeMap);
    return gen.generate(node).moveValue();
}

BSONObj ExplainGenerator::explainBSONObj(const ABT& node,
                                         const bool displayProperties,
                                         const cascades::Memo* memo,
                                         const NodeToGroupPropsMap& nodeMap) {
    auto [tag, val] =
        optimizer::ExplainGenerator::explainBSON(node, displayProperties, memo, nodeMap);
    uassert(6624070, "Expected an object", tag == sbe::value::TypeTags::Object);
    sbe::value::ValueGuard vg(tag, val);

    BSONObjBuilder builder;
    sbe::bson::convertToBsonObj(builder, sbe::value::getObjectView(val));
    return builder.done().getOwned();
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

std::string ExplainGenerator::printBSON(const sbe::value::TypeTags tag,
                                        const sbe::value::Value val) {
    ExplainPrinterImpl<ExplainVersion::V2> printer;
    printBSONstr(printer, tag, val);
    return printer.str();
}

std::string ExplainGenerator::explainLogicalProps(const std::string& description,
                                                  const properties::LogicalProps& props) {
    return ExplainGeneratorTransporter<ExplainVersion::V2>::printLogicalProps(description, props)
        .str();
}

std::string ExplainGenerator::explainPhysProps(const std::string& description,
                                               const properties::PhysProps& props) {
    return ExplainGeneratorTransporter<ExplainVersion::V2>::printPhysProps(description, props)
        .str();
}

std::string ExplainGenerator::explainMemo(const cascades::Memo& memo) {
    ExplainGeneratorTransporter<ExplainVersion::V2> gen(false /*displayProperties*/, &memo);
    return gen.printMemo().str();
}

std::pair<sbe::value::TypeTags, sbe::value::Value> ExplainGenerator::explainMemoBSON(
    const cascades::Memo& memo) {
    ExplainGeneratorTransporter<ExplainVersion::V3> gen(false /*displayProperties*/, &memo);
    return gen.printMemo().moveValue();
}

std::string ExplainGenerator::explainPartialSchemaReqMap(const PartialSchemaRequirements& reqMap) {
    ExplainGeneratorTransporter<ExplainVersion::V2> gen;
    ExplainGeneratorTransporter<ExplainVersion::V2>::ExplainPrinter result;
    gen.printPartialSchemaReqMap(result, reqMap);
    return result.str();
}

std::string ExplainGenerator::explainInterval(const IntervalRequirement& interval) {
    ExplainGeneratorTransporter<ExplainVersion::V2> gen;
    return gen.printInterval(interval);
}

std::string ExplainGenerator::explainIntervalExpr(const IntervalReqExpr::Node& intervalExpr) {
    ExplainGeneratorTransporter<ExplainVersion::V2> gen;
    return gen.printIntervalExpr(intervalExpr).str();
}

}  // namespace mongo::optimizer
