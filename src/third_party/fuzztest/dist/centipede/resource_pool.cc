// Copyright 2024 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/resource_pool.h"

#include <sstream>
#include <string>
#include <thread>  // NOLINT: For thread IDs.
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/rusage_stats.h"

namespace fuzztest::internal {

template <typename ResourceT>
ResourcePool<ResourceT>::LeaseToken::LeaseToken(  //
    ResourcePool& leaser, LeaseRequest request)
    : leaser_{leaser}, request_{std::move(request)} {}

template <typename ResourceT>
ResourcePool<ResourceT>::LeaseToken::LeaseToken(  //
    ResourcePool& leaser, LeaseRequest request, absl::Status error)
    : leaser_{leaser},
      request_{std::move(request)},
      status_{std::move(error)} {}

template <typename ResourceT>
ResourcePool<ResourceT>::LeaseToken::~LeaseToken() {
  CHECK(status_checked_)  //
      << "status() was never consulted by caller: " << *this;
  if (status_.ok()) {
    leaser_.ReturnLease(*this);
  }
}

template <typename ResourceT>
const typename ResourcePool<ResourceT>::LeaseRequest&
ResourcePool<ResourceT>::LeaseToken::request() const {
  return request_;
}

template <typename ResourceT>
const absl::Status& ResourcePool<ResourceT>::LeaseToken::status() const {
  status_checked_ = true;
  return status_;
}

template <typename ResourceT>
std::string ResourcePool<ResourceT>::LeaseToken::id() const {
  std::stringstream ss;
  ss << thread_id_;
  return absl::StrCat("lease_tid_", ss.str(), "_rid_", request_.id);
}

template <typename ResourceT>
std::thread::id ResourcePool<ResourceT>::LeaseToken::thread_id() const {
  return thread_id_;
}

template <typename ResourceT>
absl::Time ResourcePool<ResourceT>::LeaseToken::created_at() const {
  return created_at_;
}

template <typename ResourceT>
absl::Duration ResourcePool<ResourceT>::LeaseToken::age() const {
  return absl::Now() - created_at_;
}

template <typename ResourceT>
ResourcePool<ResourceT>::ResourcePool(const ResourceT& quota)
    : quota_{quota}, pool_{quota} {
  LOG(INFO) << "Creating pool with quota=[" << quota.ShortStr() << "]";
}

template <typename ResourceT>
typename ResourcePool<ResourceT>::LeaseToken
ResourcePool<ResourceT>::AcquireLeaseBlocking(LeaseRequest&& request) {
  if (ABSL_VLOG_IS_ON(1)) {
    absl::ReaderMutexLock lock{&pool_mu_};
    VLOG(1) << "Received lease request " << request.id           //
            << "\nrequested: " << request.amount.FormattedStr()  //
            << "\nquota:     " << quota_.FormattedStr()          //
            << "\navailable: " << pool_.FormattedStr();
  }

  if (request.amount == ResourceT::Zero()) {
    absl::Status error =                          //
        absl::InvalidArgumentError(absl::StrCat(  //
            "Invalid lease request ", request.id, ": amount is zero"));
    return LeaseToken{*this, std::move(request), std::move(error)};
  }
  // NOTE: Using `amount > quota` would be semantically wrong, because it is
  // true only when _all_ components of `amount` are strictly greater than their
  // counterparts in `quota_`.
  if (!(request.amount <= quota_)) {
    absl::Status error =                            //
        absl::ResourceExhaustedError(absl::StrCat(  //
            "Invalid lease request ", request.id, ": amount exceeds quota: [",
            request.amount.ShortStr(), "] vs [", quota_.ShortStr(), "]"));
    return LeaseToken{*this, std::move(request), std::move(error)};
  }

  const auto got_enough_free_pool = [this, &request]() {
    pool_mu_.AssertReaderHeld();
    const bool got_pool = request.amount <= pool_;
    if (!got_pool) {
      VLOG(10)                                                     //
          << "Pending lease '" << request.id << "':"               //
          << "\nreq age   : " << request.age()                     //
          << "\navailable : " << pool_.FormattedStr()              //
          << "\nrequested : " << (-request.amount).FormattedStr()  //
          << "\nmissing   : " << (pool_ - request.amount).FormattedStr();
    }
    return got_pool;
  };

  // Block and wait until enough of the pool becomes available to satisfy
  // this request, then acquire the mutex and proceed to the true-branch. If
  // the timeout is reached, proceed to the else-branch.
  if (pool_mu_.LockWhenWithTimeout(  //
          absl::Condition{&got_enough_free_pool}, request.timeout)) {
    VLOG(1)                                                    //
        << "Granting lease " << request.id                     //
        << "\nreq age : " << request.age()                     //
        << "\nbefore  : " << pool_.FormattedStr()              //
        << "\nleased  : " << (-request.amount).FormattedStr()  //
        << "\nafter   : " << (pool_ - request.amount).FormattedStr();
    pool_ = pool_ - request.amount;
    pool_mu_.Unlock();
    return LeaseToken{*this, std::move(request)};
  } else {
    absl::Status error =                           //
        absl::DeadlineExceededError(absl::StrCat(  //
            "Lease request ", request.id, " timed out; timeout: ",
            request.timeout, " requested: [", request.amount.ShortStr(),
            "] current pool: [", pool_.ShortStr(), "]"));
    pool_mu_.Unlock();
    return LeaseToken{*this, std::move(request), std::move(error)};
  }
}

template <typename ResourceT>
void ResourcePool<ResourceT>::ReturnLease(const LeaseToken& lease) {
  absl::WriterMutexLock lock{&pool_mu_};
  VLOG(1)                                                              //
      << "Returning lease " << lease.request().id                      //
      << "\nreq age   : " << lease.request().age()                     //
      << "\nlease age : " << lease.age()                               //
      << "\nbefore    : " << pool_.FormattedStr()                      //
      << "\nreturned  : " << (+lease.request().amount).FormattedStr()  //
      << "\nafter     : " << (pool_ + lease.request().amount).FormattedStr();
  pool_ = pool_ + lease.request().amount;
}

// Explicit instantiations for the currently supported `ResourceT`s.
template class ResourcePool<RUsageMemory>;
template class ResourcePool<RUsageTiming>;

}  // namespace fuzztest::internal
