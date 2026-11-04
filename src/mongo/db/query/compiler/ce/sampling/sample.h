// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/compiler/ce/sampling/persistent_sample_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::ce {

/**
 * A loaded sample: contains the sampled documents plus metadata describing how the sample was
 * obtained. Read-only after construction.
 */
class Sample {
public:
    // Factory for OTF sample
    static Sample makeOnTheFly(std::vector<BSONObj> docs,
                               SamplingTechniqueEnum technique,
                               boost::optional<int> numChunks = boost::none) {
        return Sample(std::move(docs),
                      technique,
                      /*isPersisted=*/false,
                      /*createdAt=*/Date_t::now(),
                      /*numPages=*/boost::none,
                      numChunks);
    }

    // Factory for persistent sample
    static Sample makePersisted(std::vector<BSONObj> docs,
                                SamplingTechniqueEnum technique,
                                Date_t createdAt,
                                size_t numPages,
                                boost::optional<int> numChunks = boost::none) {
        return Sample(
            std::move(docs), technique, /*isPersisted=*/true, createdAt, numPages, numChunks);
    }

    const std::vector<BSONObj>& docs() const {
        return _docs;
    }

    size_t size() const {
        return _docs.size();
    }

    bool isPersisted() const {
        return _isPersisted;
    }

    /**
     * The technique that actually produced this sample.
     */
    SamplingTechniqueEnum technique() const {
        return _techniqueUsed;
    }

    /**
     * When the sample was collected. For a persisted sample this is when it was originally
     * generated, not when it was read back.
     */
    Date_t createdAt() const {
        return _createdAt;
    }

    /**
     * Number of page documents the persisted sample was reassembled from. Unset for OTF samples.
     */
    const boost::optional<size_t>& numPages() const {
        return _numPages;
    }

    /**
     * Number of chunks a chunk sample was generated with. Unset for samples of other techniques.
     */
    const boost::optional<int>& numChunks() const {
        return _numChunks;
    }

private:
    Sample(std::vector<BSONObj> docs,
           SamplingTechniqueEnum technique,
           bool isPersisted,
           Date_t createdAt,
           boost::optional<size_t> numPages,
           boost::optional<int> numChunks = boost::none)
        : _docs(std::move(docs)),
          _techniqueUsed(technique),
          _isPersisted(isPersisted),
          _createdAt(createdAt),
          _numPages(numPages),
          _numChunks(numChunks) {}

    std::vector<BSONObj> _docs;
    SamplingTechniqueEnum _techniqueUsed;
    bool _isPersisted;
    Date_t _createdAt;
    boost::optional<size_t> _numPages;
    boost::optional<int> _numChunks;
};

}  // namespace mongo::ce
