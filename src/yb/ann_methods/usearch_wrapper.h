// Copyright (c) YugabyteDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#pragma once

#include <cstddef>
#include <memory>

#include "yb/common/common.pb.h"

#include "yb/hnsw/hnsw_fwd.h"

#include "yb/util/result.h"
#include "yb/util/status.h"

#include "yb/vector_index/distance.h"
#include "yb/vector_index/hnsw_options.h"
#include "yb/vector_index/coordinate_types.h"
#include "yb/vector_index/vector_index_if.h"
#include "yb/vector_index/vector_index_wrapper_util.h"

namespace yb::ann_methods {

template<vector_index::IndexableVectorType Vector,
         vector_index::ValidDistanceResultType DistanceResult>
class UsearchIndexFactory {
 public:
  static vector_index::VectorIndexIfPtr<Vector, DistanceResult> Create(
      vector_index::FactoryMode mode, const hnsw::BlockCachePtr& block_cache,
      const vector_index::HNSWOptions& options, HnswBackend backend,
      const std::shared_ptr<MemTracker>& mem_tracker);
};

template<vector_index::IndexableVectorType Vector,
         vector_index::ValidDistanceResultType DistanceResult>
class SimplifiedUsearchIndexFactory {
 public:
  static vector_index::VectorIndexIfPtr<Vector, DistanceResult> Create(
      vector_index::FactoryMode mode, const vector_index::HNSWOptions& options) {
    return UsearchIndexFactory<Vector, DistanceResult>::Create(
        mode, nullptr, options, HnswBackend::USEARCH, nullptr);
  }
};

}  // namespace yb::ann_methods
