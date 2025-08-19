/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef avro_Layout_hh__
#define avro_Layout_hh__

#include "Config.hh"
#include <boost/noncopyable.hpp>

/// \file Layout.hh
///

namespace avro {

class AVRO_DECL Layout : private boost::noncopyable {
protected:
    explicit Layout(size_t offset = 0) : offset_(offset) {}

public:
    size_t offset() const {
        return offset_;
    }
    virtual ~Layout() = default;

private:
    const size_t offset_;
};

class AVRO_DECL PrimitiveLayout : public Layout {
public:
    explicit PrimitiveLayout(size_t offset = 0) : Layout(offset) {}
};

class AVRO_DECL CompoundLayout : public Layout {

public:
    explicit CompoundLayout(size_t offset = 0) : Layout(offset) {}

    void add(std::unique_ptr<Layout> &layout) {
        layouts_.push_back(std::move(layout));
    }

    const Layout &at(size_t idx) const {
        return *layouts_.at(idx);
    }

private:
    std::vector<std::unique_ptr<Layout>> layouts_;
};

} // namespace avro

#endif
