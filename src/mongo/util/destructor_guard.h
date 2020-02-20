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

#pragma once

#include "mongo/logv2/log_detail.h"
#include "mongo/util/assert_util.h"

#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD
#define MONGO_DESTRUCTOR_GUARD(expression)                             \
    try {                                                              \
        expression;                                                    \
    } catch (const std::exception& e) {                                \
        logv2::detail::doLog(4615600,                                  \
                             logv2::LogSeverity::Log(),                \
                             {logv2::LogComponent::kDefault},          \
                             "caught exception in destructor",         \
                             "exception"_attr = e.what(),              \
                             "function"_attr = __FUNCTION__);          \
    } catch (...) {                                                    \
        logv2::detail::doLog(4615601,                                  \
                             logv2::LogSeverity::Log(),                \
                             {logv2::LogComponent::kDefault},          \
                             "caught unknown exception in destructor", \
                             "function"_attr = __FUNCTION__);          \
    }
