// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

const boost::log::attribute_name& tenant() {
    static const boost::log::attribute_name attr("tenant");
    return attr;
}

const boost::log::attribute_name& component() {
    static const boost::log::attribute_name attr("component");
    return attr;
}

const boost::log::attribute_name& service() {
    static const boost::log::attribute_name attr("service");
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

const boost::log::attribute_name& devStacktrace() {
    static const boost::log::attribute_name attr("devStacktraces");
    return attr;
}

}  // namespace mongo::logv2::attributes
