/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/util/printable_enum.h"

#define KEYFIELDNAMES(ENUMIFY)   \
    ENUMIFY(add)                 \
    ENUMIFY(atan2)               \
    ENUMIFY(abs)                 \
    ENUMIFY(ceil)                \
    ENUMIFY(divide)              \
    ENUMIFY(exponent)            \
    ENUMIFY(floor)               \
    ENUMIFY(ln)                  \
    ENUMIFY(log)                 \
    ENUMIFY(logten)              \
    ENUMIFY(mod)                 \
    ENUMIFY(multiply)            \
    ENUMIFY(pow)                 \
    ENUMIFY(round)               \
    ENUMIFY(sqrt)                \
    ENUMIFY(subtract)            \
    ENUMIFY(trunc)               \
    ENUMIFY(id)                  \
    ENUMIFY(andExpr)             \
    ENUMIFY(orExpr)              \
    ENUMIFY(notExpr)             \
    ENUMIFY(cmp)                 \
    ENUMIFY(eq)                  \
    ENUMIFY(gt)                  \
    ENUMIFY(gte)                 \
    ENUMIFY(lt)                  \
    ENUMIFY(lte)                 \
    ENUMIFY(ne)                  \
    ENUMIFY(project)             \
    ENUMIFY(inhibitOptimization) \
    ENUMIFY(unionWith)           \
    ENUMIFY(collArg)             \
    ENUMIFY(pipelineArg)         \
    ENUMIFY(sample)              \
    ENUMIFY(sizeArg)             \
    ENUMIFY(skip)                \
    ENUMIFY(limit)               \
    ENUMIFY(constExpr)           \
    ENUMIFY(literal)             \
    ENUMIFY(convert)             \
    ENUMIFY(inputArg)            \
    ENUMIFY(toArg)               \
    ENUMIFY(onErrorArg)          \
    ENUMIFY(onNullArg)           \
    ENUMIFY(toBool)              \
    ENUMIFY(toDate)              \
    ENUMIFY(toDecimal)           \
    ENUMIFY(toDouble)            \
    ENUMIFY(toInt)               \
    ENUMIFY(toLong)              \
    ENUMIFY(toObjectId)          \
    ENUMIFY(toString)            \
    ENUMIFY(type)

MAKE_PRINTABLE_ENUM(KeyFieldname, KEYFIELDNAMES);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(key_fieldname, KeyFieldname, KEYFIELDNAMES);
#undef FIELDNAMES
