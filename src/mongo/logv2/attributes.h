// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <boost/log/attributes/attribute_name.hpp>

namespace mongo::logv2 {
namespace [[MONGO_MOD_PUBLIC]] attributes {

// Reusable attribute names, so they only need to be constructed once.
const boost::log::attribute_name& domain();
const boost::log::attribute_name& severity();
const boost::log::attribute_name& tenant();
const boost::log::attribute_name& component();
const boost::log::attribute_name& service();
const boost::log::attribute_name& timeStamp();
const boost::log::attribute_name& threadName();
const boost::log::attribute_name& tags();
const boost::log::attribute_name& id();
const boost::log::attribute_name& message();
const boost::log::attribute_name& attributes();
const boost::log::attribute_name& truncation();
const boost::log::attribute_name& userassert();
const boost::log::attribute_name& devStacktrace();

}  // namespace attributes
}  // namespace mongo::logv2
