// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

// Equatorial radius of earth.
// Source: http://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
const double kRadiusOfEarthInMeters = (6378.1 * 1000);

}  // namespace mongo
