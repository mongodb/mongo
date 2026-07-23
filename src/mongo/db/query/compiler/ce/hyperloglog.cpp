// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/hyperloglog.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace mongo::ce {
namespace {

// Correction factor for the systematic multiplicative bias of the raw estimate, see Flajolet et
// al. (2007), theorem 1 and figure 3.
double alpha(size_t numRegisters) {
    switch (numRegisters) {
        case 16:
            return 0.673;
        case 32:
            return 0.697;
        case 64:
            return 0.709;
        default:
            return 0.7213 / (1 + 1.079 / numRegisters);
    }
}

Status validatePrecision(size_t precision) {
    if (precision < HyperLogLog::kMinPrecision || precision > HyperLogLog::kMaxPrecision) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "HyperLogLog precision must be in [" << HyperLogLog::kMinPrecision
                          << ", " << HyperLogLog::kMaxPrecision << "], got " << precision);
    }
    return Status::OK();
}

}  // namespace

StatusWith<HyperLogLog> HyperLogLog::create(size_t precision) {
    if (auto status = validatePrecision(precision); !status.isOK()) {
        return status;
    }
    return HyperLogLog(precision);
}

StatusWith<HyperLogLog> HyperLogLog::create(size_t precision, Registers registers) {
    if (auto status = validatePrecision(precision); !status.isOK()) {
        return status;
    }
    if (registers.size() != size_t{1} << precision) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Serialized HyperLogLog register array must hold exactly 2^precision "
                             "registers, got "
                          << registers.size() << " for precision " << precision);
    }
    const size_t maxRank = 64 - precision + 1;
    for (uint8_t reg : registers) {
        if (reg > maxRank) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Serialized HyperLogLog register value " << +reg
                                        << " exceeds the maximum rank " << maxRank
                                        << " for precision " << precision);
        }
    }
    return HyperLogLog(precision, registers);
}

HyperLogLog::HyperLogLog(size_t precision) : _precision(precision) {
    _registers.resize(size_t{1} << precision, 0);
}

HyperLogLog::HyperLogLog(size_t precision, Registers registers)
    : _registers(registers.begin(), registers.end()), _precision(precision) {}

void HyperLogLog::addHash(uint64_t hash) {
    const size_t index = hash >> (64 - _precision);
    // The rank is the position of the leftmost 1-bit in the bits not used for register selection:
    // 1 if the first bit is set, up to q + 1 if all q remaining bits are zero.
    // std::countl_zero(0) == 64 makes the all-zero case fall out of the clamp.
    const uint64_t rest = hash << _precision;
    const size_t rank = std::min<size_t>(std::countl_zero(rest) + 1, 64 - _precision + 1);
    _registers[index] = std::max(_registers[index], static_cast<uint8_t>(rank));
}

double HyperLogLog::estimate() const {
    // Bucket the registers by value; a register never exceeds 64 - precision + 1.
    std::array<uint32_t, 65> counts{};
    for (uint8_t reg : _registers) {
        ++counts[reg];
    }

    // The raw estimate is the bias-corrected harmonic mean of the per-register estimates 2^reg,
    // scaled by the number of registers.
    double invSum = 0;
    for (size_t k = 0; k < counts.size(); ++k) {
        invSum += std::ldexp(static_cast<double>(counts[k]), -static_cast<int>(k));
    }
    const double m = static_cast<double>(_registers.size());
    // 'invSum' cannot be zero: every register contributes at least 2^-61 (register values are
    // bounded by 64 - kMinPrecision + 1), so invSum >= m * 2^-61 > 0.
    tassert(11207606, "HyperLogLog register sum must be positive", invSum > 0);
    const double raw = alpha(_registers.size()) * m * m / invSum;

    // Small range correction: for cardinalities well below the register count the raw estimate is
    // biased, while the number of still-empty registers is a near-exact predictor (linear
    // counting, see Flajolet et al. (2007), section 4).
    if (raw <= 2.5 * m && counts[0] > 0) {
        return m * std::log(m / counts[0]);
    }
    return raw;
}

void HyperLogLog::merge(const HyperLogLog& other) {
    tassert(11207601,
            "Cannot merge HyperLogLog sketches with different precisions",
            _precision == other._precision);
    for (size_t i = 0; i < _registers.size(); ++i) {
        _registers[i] = std::max(_registers[i], other._registers[i]);
    }
}

}  // namespace mongo::ce
