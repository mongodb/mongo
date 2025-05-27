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
#include "mongo/db/exec/sbe/vm/vm_printer.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/code_fragment.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_instruction.h"
#include "mongo/db/query/datetime/date_time_support.h"

#include <algorithm>
#include <iomanip>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

namespace mongo::sbe::vm {

template <CodeFragmentPrinter::PrintFormat format>
struct CodeFragmentFormatter;


template <>
struct CodeFragmentFormatter<CodeFragmentPrinter::PrintFormat::Debug> {
    auto pcPointer(const uint8_t* pcPointer) {
        return PcPointerFmt{pcPointer};
    }

    auto slotAccessor(value::SlotAccessor* accessor) {
        return SlotAccessorFmt{accessor};
    }

    struct PcPointerFmt {
        const uint8_t* pcPointer;
    };

    struct SlotAccessorFmt {
        value::SlotAccessor* accessor;
    };
};

template <typename charT, typename traits>
std::basic_ostream<charT, traits>& operator<<(
    std::basic_ostream<charT, traits>& os,
    const CodeFragmentFormatter<CodeFragmentPrinter::PrintFormat::Debug>::PcPointerFmt& a) {
    return os << static_cast<const void*>(a.pcPointer);
}

template <typename charT, typename traits>
std::basic_ostream<charT, traits>& operator<<(
    std::basic_ostream<charT, traits>& os,
    const CodeFragmentFormatter<CodeFragmentPrinter::PrintFormat::Debug>::SlotAccessorFmt& a) {
    return os << static_cast<const void*>(a.accessor);
}

template <>
struct CodeFragmentFormatter<CodeFragmentPrinter::PrintFormat::Stable> {
    CodeFragmentFormatter(const CodeFragment& code) : code(code) {}

    auto pcPointer(const uint8_t* pcPointer) {
        return PcPointerFmt{pcPointer, code.instrs().data()};
    }

    auto slotAccessor(value::SlotAccessor* accessor) {
        return SlotAccessorFmt{accessor};
    }

    struct PcPointerFmt {
        const uint8_t* pcPointer;
        const uint8_t* pcBegin;
    };

    struct SlotAccessorFmt {
        value::SlotAccessor* accessor;
    };

    const CodeFragment& code;
};

template <typename charT, typename traits>
std::basic_ostream<charT, traits>& operator<<(
    std::basic_ostream<charT, traits>& os,
    const CodeFragmentFormatter<CodeFragmentPrinter::PrintFormat::Stable>::PcPointerFmt& a) {

    auto flags = os.flags();
    os << "0x" << std::hex << std::setw(4) << std::setfill('0') << (a.pcPointer - a.pcBegin);
    os.flags(flags);
    return os;
}

template <typename charT, typename traits>
std::basic_ostream<charT, traits>& operator<<(
    std::basic_ostream<charT, traits>& os,
    const CodeFragmentFormatter<CodeFragmentPrinter::PrintFormat::Stable>::SlotAccessorFmt& a) {
    return os << "<accessor>";
}

template <typename Formatter>
class CodeFragmentPrinterImpl {
public:
    CodeFragmentPrinterImpl(Formatter formatter) : _formatter(formatter) {}

    template <typename charT, typename traits>
    void print(std::basic_ostream<charT, traits>& os, const CodeFragment& code) {
        auto pcBegin = code.instrs().data();
        auto pcEnd = pcBegin + code.instrs().size();

        // Prints code address range, delta stack size, and max stack size for this CodeFragment.
        os << "[" << _formatter.pcPointer(pcBegin) << "-" << _formatter.pcPointer(pcEnd) << "]"
           << " stackSize: " << code.stackSize() << ", maxStackSize: " << code.maxStackSize()
           << "\n";

        auto pcPointer = pcBegin;
        auto sortedFixupOffsets = getSortedFixupOffsetsWithFrameIds(code);
        auto nextFixup = sortedFixupOffsets.begin();
        while (pcPointer < pcEnd) {
            Instruction i = readFromMemory<Instruction>(pcPointer);
            os << _formatter.pcPointer(pcPointer) << ": " << i.toString() << "(";
            pcPointer += sizeof(i);
            switch (i.tag) {
                // Instructions with no arguments.
                case Instruction::pop:
                case Instruction::swap:
                case Instruction::fillEmpty:
                case Instruction::traverseP:
                case Instruction::traverseF:
                case Instruction::setField:
                case Instruction::aggSum:
                case Instruction::aggMin:
                case Instruction::aggCollMin:
                case Instruction::aggMax:
                case Instruction::aggCollMax:
                case Instruction::aggFirst:
                case Instruction::aggLast:
                case Instruction::fail:
                case Instruction::ret: {
                    break;
                }
                case Instruction::allocStack: {
                    auto size = readFromMemory<uint32_t>(pcPointer);
                    pcPointer += sizeof(size);
                    os << "size:" << size;
                    break;
                }

                // Instructions with 2 arguments and a collator.
                case Instruction::collLess:
                case Instruction::collLessEq:
                case Instruction::collGreater:
                case Instruction::collGreaterEq:
                case Instruction::collEq:
                case Instruction::collNeq:
                case Instruction::collCmp3w: {
                    auto [popColl, moveFromColl, offsetColl] =
                        Instruction::Parameter::decodeParam(pcPointer);
                    auto [popLhs, moveFromLhs, offsetLhs] =
                        Instruction::Parameter::decodeParam(pcPointer);
                    auto [popRhs, moveFromRhs, offsetRhs] =
                        Instruction::Parameter::decodeParam(pcPointer);

                    os << "popLhs: " << popLhs << ", moveFromLhs: " << moveFromLhs
                       << ", offsetLhs: " << offsetLhs << ", popRhs: " << popRhs
                       << ", moveFromRhs: " << moveFromRhs << ", offsetRhs: " << offsetRhs
                       << ", popColl: " << popColl << ", moveFromColl: " << moveFromColl
                       << ", offsetColl: " << offsetColl;
                    break;
                }
                // Instructions with 2 arguments.
                case Instruction::add:
                case Instruction::sub:
                case Instruction::mul:
                case Instruction::div:
                case Instruction::idiv:
                case Instruction::mod:
                case Instruction::less:
                case Instruction::lessEq:
                case Instruction::greater:
                case Instruction::greaterEq:
                case Instruction::eq:
                case Instruction::neq:
                case Instruction::cmp3w:
                case Instruction::getField:
                case Instruction::getElement:
                case Instruction::collComparisonKey:
                case Instruction::getFieldOrElement: {
                    auto [popLhs, moveFromLhs, offsetLhs] =
                        Instruction::Parameter::decodeParam(pcPointer);
                    auto [popRhs, moveFromRhs, offsetRhs] =
                        Instruction::Parameter::decodeParam(pcPointer);

                    os << "popLhs: " << popLhs << ", moveFromLhs: " << moveFromLhs
                       << ", offsetLhs: " << offsetLhs << ", popRhs: " << popRhs
                       << ", moveFromRhs: " << moveFromRhs << ", offsetRhs: " << offsetRhs;
                    break;
                }
                // Instructions with 1 argument.
                case Instruction::makeOwn:
                case Instruction::negate:
                case Instruction::logicNot:
                case Instruction::getArraySize:
                case Instruction::exists:
                case Instruction::isNull:
                case Instruction::isObject:
                case Instruction::isArray:
                case Instruction::isInList:
                case Instruction::isString:
                case Instruction::isNumber:
                case Instruction::isBinData:
                case Instruction::isDate:
                case Instruction::isNaN:
                case Instruction::isInfinity:
                case Instruction::isRecordId:
                case Instruction::isMinKey:
                case Instruction::isMaxKey:
                case Instruction::isTimestamp: {
                    auto [popParam, moveFromParam, offsetParam] =
                        Instruction::Parameter::decodeParam(pcPointer);
                    os << "popParam: " << popParam << ", moveFromParam: " << moveFromParam
                       << ", offsetParam: " << offsetParam;
                    break;
                }
                // Instructions with a single integer argument.
                case Instruction::pushLocalLambda: {
                    auto offset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(offset);
                    os << "target: " << _formatter.pcPointer(pcPointer + offset);
                    break;
                }
                case Instruction::pushLocalVal:
                case Instruction::pushMoveLocalVal: {
                    auto arg = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(arg);
                    os << "arg: " << arg;
                    break;
                }
                case Instruction::jmp:
                case Instruction::jmpTrue:
                case Instruction::jmpFalse:
                case Instruction::jmpNothing:
                case Instruction::jmpNotNothing: {
                    auto offset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(offset);
                    os << "target: " << _formatter.pcPointer(pcPointer + offset);
                    break;
                }
                // Instructions with other kinds of arguments.
                case Instruction::traversePImm:
                case Instruction::traverseFImm: {
                    auto k = readFromMemory<Instruction::Constants>(pcPointer);
                    pcPointer += sizeof(k);
                    auto offset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(offset);
                    os << "k: " << Instruction::toStringConstants(k)
                       << ", target: " << _formatter.pcPointer(pcPointer + offset);
                    break;
                }
                case Instruction::fillEmptyImm: {
                    auto k = readFromMemory<Instruction::Constants>(pcPointer);
                    pcPointer += sizeof(k);
                    os << "k: " << Instruction::toStringConstants(k);
                    break;
                }
                case Instruction::getFieldImm: {
                    auto [popParam, moveFromParam, offsetParam] =
                        Instruction::Parameter::decodeParam(pcPointer);
                    auto size = readFromMemory<uint8_t>(pcPointer);
                    pcPointer += sizeof(size);
                    StringData fieldName(reinterpret_cast<const char*>(pcPointer), size);
                    pcPointer += size;

                    os << "popParam: " << popParam << ", moveFromParam: " << moveFromParam
                       << ", offsetParam: " << offsetParam << ", value: \"" << fieldName << "\"";
                    break;
                }
                case Instruction::pushConstVal: {
                    auto tag = readFromMemory<value::TypeTags>(pcPointer);
                    pcPointer += sizeof(tag);
                    auto val = readFromMemory<value::Value>(pcPointer);
                    pcPointer += sizeof(val);
                    os << "value: ";
                    value::ValuePrinters::make(
                        os, PrintOptions().useTagForAmbiguousValues(true).normalizeOutput(true))
                        .writeValueToStream(tag, val);
                    break;
                }
                case Instruction::pushOwnedAccessorVal:
                case Instruction::pushEnvAccessorVal:
                case Instruction::pushAccessVal:
                case Instruction::pushMoveVal: {
                    auto accessor = readFromMemory<value::SlotAccessor*>(pcPointer);
                    pcPointer += sizeof(accessor);
                    os << "accessor: " << _formatter.slotAccessor(accessor);
                    break;
                }
                case Instruction::numConvert: {
                    auto tag = readFromMemory<value::TypeTags>(pcPointer);
                    pcPointer += sizeof(tag);
                    os << "tag: " << tag;
                    break;
                }
                case Instruction::typeMatchImm: {
                    auto [popParam, moveFromParam, offsetParam] =
                        Instruction::Parameter::decodeParam(pcPointer);
                    auto mask = readFromMemory<uint32_t>(pcPointer);
                    pcPointer += sizeof(mask);
                    os << "popParam: " << popParam << ", moveFromParam: " << moveFromParam
                       << ", offsetParam: " << offsetParam << ", mask: " << mask;
                    break;
                }
                case Instruction::functionSmall: {
                    auto f = readFromMemory<SmallBuiltinType>(pcPointer);
                    pcPointer += sizeof(f);
                    ArityType arity{0};
                    arity = readFromMemory<SmallArityType>(pcPointer);
                    pcPointer += sizeof(SmallArityType);
                    os << "f: " << builtinToString(static_cast<Builtin>(f)) << ", arity: " << arity;
                    break;
                }
                case Instruction::function: {
                    auto f = readFromMemory<Builtin>(pcPointer);
                    pcPointer += sizeof(f);
                    ArityType arity{0};
                    arity = readFromMemory<ArityType>(pcPointer);
                    pcPointer += sizeof(ArityType);
                    os << "f: " << builtinToString(f) << ", arity: " << arity;
                    break;
                }
                case Instruction::dateTruncImm: {
                    auto unit = readFromMemory<TimeUnit>(pcPointer);
                    pcPointer += sizeof(unit);
                    auto binSize = readFromMemory<int64_t>(pcPointer);
                    pcPointer += sizeof(binSize);
                    auto timezone = readFromMemory<TimeZone>(pcPointer);
                    pcPointer += sizeof(timezone);
                    auto startOfWeek = readFromMemory<DayOfWeek>(pcPointer);
                    pcPointer += sizeof(startOfWeek);
                    os << "unit: " << static_cast<int32_t>(unit) << ", binSize: " << binSize
                       << ", timezone: " << timezone.toString()
                       << ", startOfWeek: " << static_cast<int32_t>(startOfWeek);
                    break;
                }
                default:
                    os << "unknown";
            }
            os << ");\n";

            // Prints any outstanding fixups in 'code' that apply to the current instruction. The
            // next instruction, if there is one, is at offset 'pcPointer'.
            while (nextFixup != sortedFixupOffsets.end() &&
                   static_cast<long>((*nextFixup)->fixupOffset) < pcPointer - pcBegin) {
                os << _formatter.pcPointer(pcBegin + (*nextFixup)->fixupOffset)
                   << ":   fixup: frameId: " << (*nextFixup)->frameId << "\n";
                ++nextFixup;
            }
        }  // while (pcPointer < pcEnd)
    }

private:
    // Used for sorting fixup offsets while remembering their FrameIds.
    struct FixupOffsetAndFrameId {
        FixupOffsetAndFrameId(size_t fix, FrameId fid) : fixupOffset(fix), frameId(fid) {}

        size_t fixupOffset;
        FrameId frameId;
    };

    // Returns a vector of FixupOffsetAndFrameId for all fixupOffsets of all frames in 'code'
    // sorted by fixupOffset.
    std::vector<std::unique_ptr<FixupOffsetAndFrameId>> getSortedFixupOffsetsWithFrameIds(
        const CodeFragment& code) const {
        auto sorted = std::vector<std::unique_ptr<FixupOffsetAndFrameId>>();  // return value

        for (const auto& frameIdInfoPair : code.frames()) {
            for (auto fixupOffset : frameIdInfoPair.second.fixupOffsets) {
                sorted.emplace_back(
                    std::make_unique<FixupOffsetAndFrameId>(fixupOffset, frameIdInfoPair.first));
            }
        }
        std::sort(sorted.begin(),
                  sorted.end(),
                  [](std::unique_ptr<FixupOffsetAndFrameId> const& a,
                     std::unique_ptr<FixupOffsetAndFrameId> const& b) {
                      return a->fixupOffset < b->fixupOffset;
                  });
        return sorted;
    }

    Formatter _formatter;
};


void CodeFragmentPrinter::print(std::ostream& os, const CodeFragment& code) const {
    switch (_format) {
        case PrintFormat::Debug:
            CodeFragmentPrinterImpl(CodeFragmentFormatter<PrintFormat::Debug>()).print(os, code);
            break;
        case PrintFormat::Stable:
            CodeFragmentPrinterImpl(CodeFragmentFormatter<PrintFormat::Stable>(code))
                .print(os, code);
            break;
    }
}
}  // namespace mongo::sbe::vm
