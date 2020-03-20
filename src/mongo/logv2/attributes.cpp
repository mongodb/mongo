/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/log/attributes/attribute_name.hpp>

namespace mongo::logv2::attributes {

const boost::log::attribute_name& domain() {
    static const boost::log::attribute_name attr("domain");
    return attr;
}

const boost::log::attribute_name& severity() {
    static const boost::log::attribute_name attr("severity");
    return attr;
}

const boost::log::attribute_name& component() {
    static const boost::log::attribute_name attr("component");
    return attr;
}

const boost::log::attribute_name& timeStamp() {
    static const boost::log::attribute_name attr("time_stamp");
    return attr;
}

const boost::log::attribute_name& threadName() {
    static const boost::log::attribute_name attr("thread_name");
    return attr;
}

const boost::log::attribute_name& tags() {
    static const boost::log::attribute_name attr("tags");
    return attr;
}

const boost::log::attribute_name& id() {
    static const boost::log::attribute_name attr("id");
    return attr;
}

const boost::log::attribute_name& message() {
    static const boost::log::attribute_name attr("message");
    return attr;
}

const boost::log::attribute_name& attributes() {
    static const boost::log::attribute_name attr("attributes");
    return attr;
}

const boost::log::attribute_name& truncation() {
    static const boost::log::attribute_name attr("truncation");
    return attr;
}

const boost::log::attribute_name& userassert() {
    static const boost::log::attribute_name attr("userassert");
    return attr;
}

}  // namespace mongo::logv2::attributes
