/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/column/bsoncolumn_expressions.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/platform/compiler.h"

#include <algorithm>

namespace mongo::bsoncolumn {
namespace {

/**
 * Walks the control bytes of a BSONColumn binary, dispatching to a visitor for each control byte
 * type. By default walks to the end sentinel and returns the position after it. Visitors that need
 * to track a result should store it as internal state.
 *
 * The boolean template parameter 'interleaved' is true when we are walking control bytes in an
 * interleaved section and is known at compile time. The top-level call uses interleaved=false; the
 * recursive call for interleaved sections uses interleaved=true. This lets visitors provide
 * separate implementations for outer vs. interleaved elements without runtime branching.
 *
 * When 'earlyExit' is true the walker checks 'visitor.shouldStop()' before each control byte and
 * returns the current position when it returns true. Callers using earlyExit must not rely on the
 * returned position equalling 'end' to assert well-formedness; only the portion of the binary
 * actually walked is validated.
 *
 * The Visitor must implement:
 *   template <bool interleaved>
 *   void onLiteral(BSONElement elem)
 *       Called for uncompressed literal elements.
 *   template <bool interleaved>
 *   void onSimple8b(const char* blockData, size_t blockSize)
 *       Called for simple8b delta blocks.
 *   void onInterleavedStart(uint8_t control, const BSONObj& refObj, uint32_t numStreams)
 *       Called when an interleaved section begins, before walking its inner control bytes.
 *   void onInterleavedEnd(uint32_t numStreams)
 *       Called after an interleaved section's inner control bytes have been walked.
 */
template <bool interleaved = false, bool earlyExit = false, typename Visitor>
const char* walkControlBytes(const char* pos, const char* end, Visitor&& visitor) {
    while (pos < end) {
        if constexpr (earlyExit) {
            if (visitor.shouldStop())
                return pos;
        }

        uint8_t control = static_cast<uint8_t>(*pos);

        if (control == stdx::to_underlying(BSONType::eoo)) {
            return pos + 1;
        } else if (bsoncolumn::isUncompressedLiteralControlByte(control)) {
            BSONElement elem(pos, 1, BSONElement::TrustedInitTag{});
            visitor.template onLiteral<interleaved>(elem);
            pos += elem.size();
        } else if (bsoncolumn::isInterleavedStartControlByte(control)) {
            if constexpr (interleaved) {
                uasserted(ErrorCodes::InvalidBSONColumn, "Invalid BSONColumn encoding");
            } else {
                BSONObj refObj(pos + 1);
                const char* innerStart = pos + 1 + refObj.objsize();
                uint32_t numStreams = numInterleavedStreams(refObj, control);
                visitor.onInterleavedStart(control, refObj, numStreams);
                pos = walkControlBytes<true, earlyExit>(innerStart, end, visitor);
                visitor.onInterleavedEnd(numStreams);
            }
        } else {
            // EOO, literal, and interleaved-start are all ruled out above — anything else is
            // treated as a simple8b control byte.
            uint8_t blocks = bsoncolumn::numSimple8bBlocksForControlByte(control);
            size_t blockSize = blocks * sizeof(uint64_t);
            ++pos;
            visitor.template onSimple8b<interleaved>(pos, blockSize);
            pos += blockSize;
        }
    }

    // Missing zero at end.
    uasserted(ErrorCodes::InvalidBSONColumn, "Invalid BSONColumn encoding");
}

/**
 * Counts the number of top-level elements in a BSONColumn. Literals and simple8b blocks both
 * contribute directly to the count. For interleaved sections, the inner control bytes encode all
 * streams back-to-back, so the raw count is divided by the number of streams to get the number of
 * top-level documents. onInterleavedStart snapshots the count before the section, and
 * onInterleavedEnd computes the delta and divides.
 */
struct CountVisitor {
    size_t& outCount;
    size_t interleavedBaseCount = 0;

    template <bool>
    void onLiteral(const BSONElement&) {
        ++outCount;
    }

    void onInterleavedStart(uint8_t, const BSONObj&, uint32_t) {
        interleavedBaseCount = outCount;
    }

    void onInterleavedEnd(uint32_t numStreams) {
        size_t interleavedCount = outCount - interleavedBaseCount;
        uassert(ErrorCodes::InvalidBSONColumn,
                "Invalid BSONColumn encoding",
                interleavedCount % numStreams == 0);
        outCount = interleavedBaseCount + interleavedCount / numStreams;
    }

    template <bool>
    void onSimple8b(const char* blockPos, size_t blockSize) {
        outCount += simple8b::count(blockPos, blockSize);
    }
};

/**
 * Determines whether a BSONColumn contains no missing values (i.e. is "dense"). The result will be
 * true if decompression would produce no missing values, and false otherwise.
 *
 * Regular (non-interleaved) path: every row is produced by a literal or by a simple8b delta applied
 * against the decoder's lastValue. A simple8b block encountered before any baseline is established
 * (no literal seen yet, no interleaved section yet) decodes as missing for every row. After a
 * literal, or after an interleaved section, the decoder has a real baseline; missings then only
 * come from explicit markers within the block, which simple8b::dense detects.
 *
 * For interleaved sections we compute an exact answer by tracking, for each logical row, how many
 * streams were missing at that row. The min-heap assigns each inner control byte to the stream
 * with the fewest values so far (ties broken by schema order) — the same rule the real decoder
 * uses. Within a stream's simple8b block we call simple8b::visitAll and, on every missing marker,
 * bump missCounts[baseRow + subIdx]. The moment any counter reaches numStreams we know every
 * stream was missing at that row and the section is not dense; 'dense' is cleared and we stop
 * early.
 */
struct DenseVisitor {
    bool seenLiteral = false;
    bool dense = true;

    // Enables walkControlBytes to exit early once we've determined the column is not dense.
    bool shouldStop() const {
        return !dense;
    }

    // Per-stream state for the min-heap walk inside interleaved sections. The reference object
    // acts as the initial literal for every stream, so a leading RLE block repeats a zero delta
    // against the reference value (not a missing).
    struct StreamState {
        size_t fieldPos;
        size_t valueCount = 0;

        bool operator>(const StreamState& other) const {
            return std::tie(valueCount, fieldPos) > std::tie(other.valueCount, other.fieldPos);
        }
    };
    std::vector<StreamState> interleavedHeap;

    // Per-logical-row count of streams that were missing at that row, within the current
    // interleaved section. Reaching numStreams at any row means every stream was missing there and
    // the section is not dense. Prefilled to 1024 zero slots at onInterleavedStart to cover the
    // typical time-series bucket; sections beyond that fall through to an amortised-doubling grow
    // path.
    std::vector<size_t> missCounts;

    template <bool interleaved>
    void onLiteral(const BSONElement&) {
        if constexpr (interleaved) {
            std::pop_heap(interleavedHeap.begin(), interleavedHeap.end(), std::greater<>{});
            ++interleavedHeap.back().valueCount;
            std::push_heap(interleavedHeap.begin(), interleavedHeap.end(), std::greater<>{});
        } else {
            seenLiteral = true;
        }
    }

    void onInterleavedStart(uint8_t, const BSONObj&, uint32_t n) {
        interleavedHeap.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            interleavedHeap.emplace_back(i);
        }
        std::make_heap(interleavedHeap.begin(), interleavedHeap.end(), std::greater<>{});
        missCounts.resize(1024, 0);
    }

    void onInterleavedEnd(uint32_t) {
        interleavedHeap.clear();
        missCounts.clear();
        // Interleaved exit carries the last decoded row as the regular-mode lastValue (see
        // BSONColumn::Iterator::_exitInterleavedMode). That baseline behaves like a literal for
        // subsequent simple8b blocks: a zero-delta (or RLE thereof) repeats it rather than
        // decoding as missing.
        seenLiteral = true;
    }

    template <bool interleaved>
    void onSimple8b(const char* blockData, size_t blockSize) {
        if constexpr (interleaved) {
            std::pop_heap(interleavedHeap.begin(), interleavedHeap.end(), std::greater<>{});
            auto& state = interleavedHeap.back();
            size_t baseRow = state.valueCount;

            // We always pass kSingleZero as prevNonRLE rather than threading it across a stream's
            // successive blocks. This can misinterpret an RLE word whose true meaning depends on
            // a prior block, but we only count missings here; the non-missing values reported by
            // visit/visitZero are discarded, so any RLE misinterpretation among non-missing
            // values is harmless.
            //
            // We instantiate visitAll with int128_t rather than int64_t so that the extended-
            // selector decode path produces correct values for streams whose logical type is
            // 128-bit (e.g. Decimal128). The int64 instantiation would truncate such slots; it
            // doesn't UB (the decoder clamps the shift), but a truncated RLE baseline could
            // coincidentally equal kMissing and make us falsely report the section as non-dense.
            // We don't know the stream's logical type here, so the wider instantiation is the
            // safe default. The values themselves are discarded.
            uint64_t prevNonRLE = simple8b::kSingleZero;
            size_t subIdx = 0;
            size_t visited = simple8b::visitAll<int128_t>(
                blockData,
                blockSize,
                prevNonRLE,
                [&subIdx](int128_t) { ++subIdx; },
                [&subIdx]() { ++subIdx; },
                [this, &subIdx, baseRow]() {
                    if (!dense) {
                        ++subIdx;
                        return;
                    }
                    size_t row = baseRow + subIdx;
                    if (MONGO_unlikely(row >= missCounts.size()))
                        missCounts.resize(std::max(row + 1, missCounts.size() * 2), 0);
                    if (++missCounts[row] == interleavedHeap.size())
                        dense = false;
                    ++subIdx;
                });

            state.valueCount += visited;
            std::push_heap(interleavedHeap.begin(), interleavedHeap.end(), std::greater<>{});
        } else {
            // A simple8b block before any literal has no baseline to delta against; every row
            // decodes as missing (the iterator's lastValue is EOO). seenLiteral is also set true
            // by onInterleavedEnd, since an interleaved section carries its last row forward as
            // the baseline for subsequent simple8b blocks. So this guard only fires for truly
            // leading simple8b — no literal and no interleaved section has run yet.
            if (!seenLiteral) {
                dense = false;
                return;
            }

            // With a baseline established, missings can only come from explicit markers within
            // the block. simple8b::dense scans for them; it correctly handles RLE within the
            // same block by short-circuiting on a kSingleSkip word before reaching the RLE.
            if (!simple8b::dense(blockData, blockSize))
                dense = false;
        }
    }
};

}  // namespace

size_t count(const char* buffer, size_t size) {
    const char* end = buffer + size;
    size_t cnt = 0;
    CountVisitor visitor{cnt};
    const char* pos = walkControlBytes(buffer, end, visitor);
    uassert(ErrorCodes::InvalidBSONColumn, "Invalid BSONColumn encoding", pos == end);
    return cnt;
}

size_t count(BSONBinData bin) {
    tassert(ErrorCodes::InvalidBSONColumn,
            "Invalid BSON type for column",
            bin.type == BinDataType::Column);
    return count(reinterpret_cast<const char*>(bin.data), bin.length);
}

bool dense(const char* buffer, size_t size) {
    const char* end = buffer + size;
    DenseVisitor visitor;
    const char* pos = walkControlBytes<false, /* earlyExit */ true>(buffer, end, visitor);
    // When the visitor short-circuits (non-dense), the remainder of the binary is intentionally
    // unchecked; only require the walker to have reached end when we walked the whole thing.
    uassert(
        ErrorCodes::InvalidBSONColumn, "Invalid BSONColumn encoding", pos == end || !visitor.dense);
    return visitor.dense;
}

bool dense(BSONBinData bin) {
    tassert(ErrorCodes::InvalidBSONColumn,
            "Invalid BSON type for column",
            bin.type == BinDataType::Column);
    return dense(reinterpret_cast<const char*>(bin.data), bin.length);
}
}  // namespace mongo::bsoncolumn
