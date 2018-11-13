// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#include "absl/time/internal/cctz/include/cctz/zone_info_source.h"

namespace absl {
namespace time_internal {
namespace cctz {

// Defined out-of-line to avoid emitting a weak vtable in all TUs.
ZoneInfoSource::~ZoneInfoSource() {}
std::string ZoneInfoSource::Version() const { return std::string(); }

}  // namespace cctz
}  // namespace time_internal
}  // namespace absl

namespace absl {
namespace time_internal {
namespace cctz_extension {

namespace {

// A default for cctz_extension::zone_info_source_factory, which simply
// defers to the fallback factory.
std::unique_ptr<absl::time_internal::cctz::ZoneInfoSource> DefaultFactory(
    const std::string& name,
    const std::function<std::unique_ptr<absl::time_internal::cctz::ZoneInfoSource>(
        const std::string& name)>& fallback_factory) {
  return fallback_factory(name);
}

}  // namespace

// A "weak" definition for cctz_extension::zone_info_source_factory.
// The user may override this with their own "strong" definition (see
// zone_info_source.h).
#if defined(_MSC_VER)
extern ZoneInfoSourceFactory zone_info_source_factory;
extern ZoneInfoSourceFactory default_factory;
ZoneInfoSourceFactory default_factory = DefaultFactory;
#if defined(_M_IX86)
#pragma comment( \
    linker,      \
    "/alternatename:?zone_info_source_factory@cctz_extension@time_internal@absl@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@5@ABV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@5@@ZA=?default_factory@cctz_extension@time_internal@absl@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@5@ABV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@5@@ZA")
#elif defined(_M_IA_64) || defined(_M_AMD64)
#pragma comment( \
    linker,      \
    "/alternatename:?zone_info_source_factory@cctz_extension@time_internal@absl@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@5@AEBV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@5@@ZEA=?default_factory@cctz_extension@time_internal@absl@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@5@AEBV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@time_internal@absl@@U?$default_delete@VZoneInfoSource@cctz@time_internal@absl@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@5@@ZEA")
#else
#error Unsupported MSVC platform
#endif
#else  // _MSC_VER
#if !defined(__has_attribute)
#define __has_attribute(x) 0
#endif
#if __has_attribute(weak) || defined(__GNUC__)
ZoneInfoSourceFactory zone_info_source_factory
    __attribute__((weak)) = DefaultFactory;
#else
// Make it a "strong" definition if we have no other choice.
ZoneInfoSourceFactory zone_info_source_factory = DefaultFactory;
#endif
#endif  // _MSC_VER

}  // namespace cctz_extension
}  // namespace time_internal
}  // namespace absl
