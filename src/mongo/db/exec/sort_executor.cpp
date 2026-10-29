// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/sort_executor.h"

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/sorter/file_based_spiller.h"
#include "mongo/db/sorter/sorter_template_defs.h"  // IWYU pragma: keep

namespace mongo {
template <typename T>
std::unique_ptr<Sorter<Value, T>> SortExecutor<T>::makeSorter() {
    auto opts = makeSortOptions();
    return Sorter<Value, T>::template make<Comparator>(
        opts,
        Comparator(_sortPattern),
        _diskUseAllowed
            ? std::make_shared<sorter::FileBasedSpiller<Value, T, Comparator>>(
                  _tempDir,
                  _sorterFileStats.get(),
                  /*dbName=*/boost::none,
                  sorter::kLatestChecksumVersion,
                  static_cast<int64_t>(internalQuerySpillingMinAvailableDiskSpaceBytes.load()))
            : nullptr,
        /*settings=*/{});
}
template class SortExecutor<Document>;
template class SortExecutor<SortableWorkingSetMember>;
template class SortExecutor<BSONObj>;
}  // namespace mongo
