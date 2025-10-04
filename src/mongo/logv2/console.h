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

#include <iosfwd>


namespace mongo::logv2 {

/**
 * Representation of the console.  Use this in place of cout/cin, in applications that write to
 * the console from multiple threads (such as those that use the logging subsystem).
 *
 * The Console type is synchronized such that only one instance may be in the fully constructed
 * state at a time.  Correct usage is to instantiate one, write or read from it as desired, and
 * then destroy it.
 *
 * The console streams accept UTF-8 encoded data, and attempt to write it to the attached
 * console faithfully.
 *
 * TODO(schwerin): If no console is attached on Windows (services), should writes here go to the
 * event logger?
 */
class Console {
public:
    Console();

    static std::ostream& out();
};

}  // namespace mongo::logv2
