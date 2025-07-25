//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pggate/pg_doc_op.h"

#include <algorithm>
#include <utility>

#include "yb/ash/wait_state.h"

#include "yb/client/yb_op.h"

#include "yb/common/pg_system_attr.h"
#include "yb/common/row_mark.h"

#include "yb/dockv/doc_key.h"

#include "yb/gutil/casts.h"
#include "yb/gutil/strings/escaping.h"

#include "yb/rpc/outbound_call.h"

#include "yb/util/lw_function.h"
#include "yb/util/random_util.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"

#include "yb/yql/pggate/pg_expr.h"
#include "yb/yql/pggate/pg_table.h"
#include "yb/yql/pggate/pg_tools.h"
#include "yb/yql/pggate/pggate_flags.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/yql/pggate/util/ybc_util.h"
#include "yb/yql/pggate/ybc_pggate.h"

DECLARE_uint64(rpc_max_message_size);
DECLARE_double(max_buffer_size_to_rpc_limit_ratio);

namespace yb::pggate {
namespace {

struct PgDocReadOpCachedHelper {
  PgTable dummy_table;
};

class PgDocReadOpCached : private PgDocReadOpCachedHelper, public PgDocOp {
 public:
  PgDocReadOpCached(const PgSession::ScopedRefPtr& pg_session, PrefetchedDataHolder data)
      : PgDocOp(pg_session, &dummy_table) {
    std::list<PgDocResult> results;
    for (const auto& d : *data) {
      results.emplace_back(d);
    }
    result_stream_ = std::make_unique<CachedPgDocResultStream>(std::move(results));
    VLOG(3) << "Created CachedPgDocResultStream";
  }

  Status FetchMoreResults() override {
    return STATUS(IllegalState, "FetchMoreResults is not valid for PgDocReadOpCached");
  }

  Status ExecuteInit(const YbcPgExecParameters* exec_params) override {
    return Status::OK();
  }

  Result<RequestSent> Execute(ForceNonBufferable force_non_bufferable) override {
    return RequestSent::kTrue;
  }

  bool IsWrite() const override {
    return false;
  }

 protected:
  Status DoPopulateByYbctidOps(const YbctidGenerator& generator, KeepOrder keep_order) override {
    return Status::OK();
  }

  Result<bool> DoCreateRequests() override {
    return STATUS(InternalError, "DoCreateRequests is not defined for PgDocReadOpCached");
  }

 private:
  Status CompleteProcessResponse() override {
    return STATUS(InternalError, "CompleteProcessResponse is not defined for PgDocReadOpCached");
  }
};

[[nodiscard]] inline ash::WaitStateCode ResolveWaitEventCode(
    TableType table_type, IsForWritePgDoc is_write) {
  switch (table_type) {
    case TableType::SYSTEM:
      return is_write ? ash::WaitStateCode::kCatalogWrite
                      : ash::WaitStateCode::kCatalogRead;
    case TableType::INDEX:
      return is_write ? ash::WaitStateCode::kIndexWrite
                      : ash::WaitStateCode::kIndexRead;
    case TableType::USER:
      return is_write ? ash::WaitStateCode::kTableWrite
                      : ash::WaitStateCode::kTableRead;
  }
  FATAL_INVALID_ENUM_VALUE(TableType, table_type);
}

Status UpdateMetricOnGettingResponse(
    const LWFunction<Status()>& getter, const PgDocResponse::MetricInfo& info,
    PgDocMetrics& metrics) {
  if (info.is_write) {
    // Update session stats instrumentation for write requests only. Writes are buffered and flushed
    // asynchronously, and thus it is not possible to correlate wait/execution times directly with
    // the request. We update instrumentation for writes exactly once, after successfully sending an
    // RPC request to the underlying storage layer.
    metrics.WriteRequest(info.table_type);
    return getter();
  }
  // Update session stats instrumentation only for read requests. Reads are executed
  // synchronously with respect to Postgres query execution, and thus it is possible to
  // correlate wait/execution times directly with the request. We update instrumentation for
  // reads exactly once, upon receiving a success response from the underlying storage layer.
  uint64_t wait_time = 0;
  RETURN_NOT_OK(metrics.CallWithDuration(getter, &wait_time));
  metrics.ReadRequest(info.table_type, wait_time);
  return Status::OK();
}

Status UpdateMetricOnRequestBuffering(
    const PgDocResponse::MetricInfo& info, PgDocMetrics& metrics) {
  DCHECK(info.is_write);
  return UpdateMetricOnGettingResponse(
      make_lw_function([]() -> Status { return Status::OK(); }), info, metrics);
}

Result<PgDocResponse::Data> GetResponse(
    PgDocResponse::FutureInfo& future_info, PgSession& session) {
  PgDocResponse::Data result;
  const auto& metric_info = future_info.metrics;

  RETURN_NOT_OK(UpdateMetricOnGettingResponse(make_lw_function(
      [&result, &future = future_info.future, &metric_info, &session] () -> Status {
        auto event_watcher = session.StartWaitEvent(
            ResolveWaitEventCode(metric_info.table_type, metric_info.is_write));
        result = VERIFY_RESULT(future.Get(session));
        return Status::OK();
      }),
      metric_info, session.metrics()));
  return result;
}

// Helper function to determine the type of relation that the given Pgsql operation is being
// performed on. This function classifies the operation into one of three buckets: system catalog,
// secondary index or user table requests.
[[nodiscard]] TableType ResolveRelationType(const PgsqlOp& op, const PgTableDesc& table) {
  if (table.schema().table_properties().is_ysql_catalog_table()) {
    // We don't distinguish between table reads and index reads for a catalog table.
    return TableType::SYSTEM;
  }

  // Check if we're making an index request.
  // Any request that lands on a secondary index table is treated as an index request, while
  // primary key lookups on the main table are treated as user table requests. No such distinction
  // is made for system catalog tables.
  // We make use of the following info to make this decision:
  // read_request().has_index_request() : is true for colocated secondary index requests.
  // table->isIndex() : is true for all secondary index writes and non-colocated secondary index
  //                    reads.
  return
      table.IsIndex() ||
      (op.is_read() && down_cast<const PgsqlReadOp&>(op).read_request().has_index_request())
          ? TableType::INDEX : TableType::USER;
}

} // namespace

PgDocResult::PgDocResult(rpc::SidecarHolder data)
    : data_(std::move(data)), current_row_idx_(0) {
  PgDocData::LoadCache(data_.second, &row_count_, &row_iterator_);
}

PgDocResult::PgDocResult(rpc::SidecarHolder data, const LWPgsqlResponsePB& response)
    : PgDocResult(std::move(data)) {
  if (!response.batch_orders().empty()) {
    const auto& orders = response.batch_orders();
    row_orders_.assign(orders.begin(), orders.end());
    DCHECK(row_orders_.size() == static_cast<size_t>(row_count_))
      << "Number of the row orders does not match the number of rows";
  }
}

int64_t PgDocResult::NextRowOrder() const {
  DCHECK(current_row_idx_ < static_cast<size_t>(row_count_));
  DCHECK(!row_orders_.empty());
  return row_orders_[current_row_idx_];
}

Status PgDocResult::WritePgTuple(const std::vector<PgFetchedTarget*>& targets, PgTuple *pg_tuple) {
  for (auto* target : targets) {
    if (PgDocData::ReadHeaderIsNull(&row_iterator_)) {
      target->SetNull(pg_tuple);
    } else {
      target->SetValue(&row_iterator_, pg_tuple);
    }
  }
  ++current_row_idx_;
  return Status::OK();
}

Status PgDocResult::ProcessYbctidEntry(Slice* data) {
  SCHECK(!PgDocData::ReadHeaderIsNull(data), InternalError, "System column ybctid cannot be NULL");
  auto data_size = PgDocData::ReadNumber<int64_t>(data);
  ybctids_.emplace_back(data->data(), data_size);
  data->remove_prefix(data_size);
  return Status::OK();
}

Status PgDocResult::ProcessEntries(std::function<Status(Slice* data)> processor) {
  // Sanity check: special rows must be unordered
  // Row orders assume that multiple PgDocResults are merge sorted row by row.
  // That contradicts ProcessEntries, which batch reads all the rows ignoring order.
  // If there is a need to order non-PG rows, consider to add a row reading function
  // like WritePgTuple.
  DCHECK(row_orders_.empty()) << "System data rows can't be ordered";
  for (int i = 0; i < row_count_; i++) {
    RETURN_NOT_OK(processor(&row_iterator_));
  }
  SCHECK(row_iterator_.empty(), IllegalState, "Unread row data");
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PgsqlResultStream::PgsqlResultStream(PgsqlOpPtr op) : op_(op) {}

PgsqlResultStream::PgsqlResultStream(std::list<PgDocResult>&& results)
    : results_queue_(std::move(results)) {}

Result<PgDocResult*> PgsqlResultStream::GetNextDocResult() {
  while (!results_queue_.empty() && results_queue_.front().is_eof()) {
    results_queue_.pop_front();
  }

  if (!results_queue_.empty()) {
    return &results_queue_.front();
  }

  RSTATUS_DCHECK(!op_ || !op_->is_active(), IllegalState, "Read from the stream requiring fetch");
  return nullptr;
}

void PgsqlResultStream::EmplaceDocResult(
    rpc::SidecarHolder&& data, const LWPgsqlResponsePB& response) {
  results_queue_.emplace_back(std::move(data), response);
  VLOG_WITH_FUNC(3) << "Operation " << (op_ ? op_->ToString() : "<empty>")
                    << " received new response with " << results_queue_.back().row_count()
                    << " rows. Now has " << results_queue_.size() << " responses in the queue";
  // immediately discard empty response
  if (results_queue_.back().is_eof()) {
    results_queue_.pop_back();
  }
}

int64_t PgsqlResultStream::NextRowOrder() {
  auto res = GetNextDocResult();
  DCHECK(res.ok());
  return (*res)->NextRowOrder();
}

StreamFetchStatus PgsqlResultStream::FetchStatus() const {
  for (const auto& result : results_queue_) {
    if (!result.is_eof()) {
      return StreamFetchStatus::kHasLocalData;
    }
  }
  return (op_ && op_->is_active()) ? StreamFetchStatus::kNeedsFetch : StreamFetchStatus::kDone;
}

PgDocResultStream::PgDocResultStream(PgDocFetchCallback fetch_func) : fetch_func_(fetch_func) {}

Result<PgDocResult*> PgDocResultStream::NextDocResult() {
  auto pgsql_op_stream = VERIFY_RESULT(NextReadStream());
  return pgsql_op_stream ? pgsql_op_stream->GetNextDocResult() : nullptr;
}

Status PgDocResultStream::EmplaceOpDocResult(
    const PgsqlOpPtr& op, rpc::SidecarHolder&& data, const LWPgsqlResponsePB& response) {
  auto& stream = VERIFY_RESULT_REF(FindReadStream(op, response));
  stream.EmplaceDocResult(std::move(data), response);
  return Status::OK();
}

Result<bool> PgDocResultStream::GetNextRow(
    const std::vector<PgFetchedTarget*>& targets, PgTuple* pg_tuple) {
  PgDocResult* result = VERIFY_RESULT(NextDocResult());
  if (result) {
    RETURN_NOT_OK(result->WritePgTuple(targets, pg_tuple));
    return true;
  }
  return false;
}

Result<bool> PgDocResultStream::ProcessEntries(std::function<Status(Slice* data)> processor) {
  auto result = VERIFY_RESULT(NextDocResult());
  if (result) {
    RETURN_NOT_OK(result->ProcessEntries(processor));
    return true;
  }
  return false;
}

// TODO: consider removal of GetNextYbctidBatch in favor of direct usage of ProcessEntries.
// Historically the ybctids array is owned by PgDocResult instance, as well as the underlaying data.
// So it is convenient to have internal processor in PgDocResult.
Result<std::optional<YbctidBatch>> PgDocResultStream::GetNextYbctidBatch(bool keep_order) {
  auto result = VERIFY_RESULT(NextDocResult());
  if (result) {
    RETURN_NOT_OK(result->ProcessEntries(
            [result](Slice* data) {
              return result->ProcessYbctidEntry(data);
            }));
    return YbctidBatch(result->ybctids(), keep_order);
  }
  return std::nullopt;
}

ParallelPgDocResultStream::ParallelPgDocResultStream(
    PgDocFetchCallback fetch_func, const std::vector<PgsqlOpPtr>& ops)
    : PgDocResultStream(fetch_func) {
  ResetOps(ops);
}

void ParallelPgDocResultStream::ResetOps(const std::vector<PgsqlOpPtr>& ops) {
  read_streams_.clear();
  for (const auto& op : ops) {
    read_streams_.emplace_back(op);
  }
}

Result<PgsqlResultStream*> ParallelPgDocResultStream::NextReadStream() {
  for (;;) {
    for (auto it = read_streams_.begin(); it != read_streams_.end();) {
      switch (it->FetchStatus()) {
        case StreamFetchStatus::kHasLocalData:
          return &*it;
        case StreamFetchStatus::kNeedsFetch:
          ++it;
          break;
        case StreamFetchStatus::kDone:
          it = read_streams_.erase(it);  // erase returns the next valid iterator
          break;
        default:
          LOG(FATAL) << "Invalid stream fetch status: " << it->FetchStatus();
      }
    }

    if (read_streams_.empty()) {
      return nullptr;
    }

    RETURN_NOT_OK(fetch_func_());
  }

  return STATUS(RuntimeError, "Unreachable statement");
}

Result<PgsqlResultStream&> ParallelPgDocResultStream::FindReadStream(
    const PgsqlOpPtr& op, const LWPgsqlResponsePB& response) {
  auto it = std::find(read_streams_.begin(), read_streams_.end(), op);
  if (it == read_streams_.end()) {
    return STATUS(RuntimeError, Format("Operation $0 not found", op));
  }
  return *it;
}

CachedPgDocResultStream::CachedPgDocResultStream(std::list<PgDocResult>&& results)
    : PgDocResultStream([]() {
          return STATUS(RuntimeError, "CachedPgDocResultStream does not fetch");
      }),
      read_stream_(std::move(results)) {}

void CachedPgDocResultStream::ResetOps(const std::vector<PgsqlOpPtr>& ops) {
  LOG(FATAL) << "Can't reset CachedPgDocResultStream";
}

Result<PgsqlResultStream&> CachedPgDocResultStream::FindReadStream(
    const PgsqlOpPtr& op, const LWPgsqlResponsePB& response) {
  return STATUS(RuntimeError, Format("Operation $0 not found", op));
}

Result<PgsqlResultStream*> CachedPgDocResultStream::NextReadStream() {
  return read_stream_.FetchStatus() == StreamFetchStatus::kDone ? nullptr : &read_stream_;
}

template <typename T>
MergingPgDocResultStream<T>::MergingPgDocResultStream(
    PgDocFetchCallback fetch_func, const std::vector<PgsqlOpPtr>& ops,
    std::function<T(PgsqlResultStream*)> get_order_fn)
    : PgDocResultStream(fetch_func), comp_({get_order_fn}), read_queue_(comp_) {
  ResetOps(ops);
}

template <typename T>
void MergingPgDocResultStream<T>::ResetOps(const std::vector<PgsqlOpPtr>& ops) {
  current_stream_ = nullptr;
  if (!read_queue_.empty()) {
    MergeSortPQ pq(comp_);
    read_queue_.swap(pq);
  }
  read_streams_.clear();
  for (auto& op : ops) {
    read_streams_.emplace_back(op);
  }
  started_ = false;
}

template <typename T>
Result<PgsqlResultStream&> MergingPgDocResultStream<T>::FindReadStream(
    const PgsqlOpPtr& op, const LWPgsqlResponsePB& response) {
  if (response.has_paging_state()) {
    // If request paginates, we don't know what it exactly means, has DocDB hit some limit and
    // stopped, but requested rows are still being fetched in order, or tablet has split and some
    // rows are to be fetched from other tablet. Here we assume the worst, that is, some requested
    // rows are missing and will come in the next response, ordered, but probably not in the order
    // continuing this response.
    // Since rows within single response are properly ordered, data from this response would be
    // valid participant of the merge sort. So we add it to the read list. And we separate them out
    // from this operation's stream, as we don't know if next page will be valid continuation.
    // Therefore this operation's stream continues to require fetch, and we won't start the merge
    // sort as of yet.
    // TODO: have DocDB to hint us. If DocDB is fetching specific rows with predefined order, it
    // is OK to enqueue the request at the operation's stream as long as DocDB has found all
    // requested rows so far in the list.
    // When we fetching from the tablet in some order that is not predefined, and is not matching
    // natural row order, (i.e. following vector index), if target tablet is split, the operation
    // should actively split, too. Original operation's range should be truncated to tablet
    // boundaries and new active operation should be created covering remaining range.
    // Large part of such split is beyond the scope of the result stream management structures.
    VLOG_WITH_FUNC(2) << "Adding split stream for operation " << op;
    read_streams_.emplace_back(nullptr);
    return read_streams_.back();
  }
  auto it = std::find(read_streams_.begin(), read_streams_.end(), op);
  if (it == read_streams_.end()) {
    return STATUS(RuntimeError, Format("Operation $0 not found", op));
  }
  return *it;
}

template <typename T>
Result<PgsqlResultStream*> MergingPgDocResultStream<T>::NextReadStream() {
  if (!started_) {
    VLOG_WITH_FUNC(2) << "Initialize merge sort of " << read_streams_.size() << " streams";
    DCHECK(current_stream_ == nullptr);
    while (true) {
      size_t num_not_ready = 0;
      for (const auto& stream : read_streams_) {
        if (stream.FetchStatus() == StreamFetchStatus::kNeedsFetch) {
          ++num_not_ready;
        }
      }
      if (num_not_ready == 0) {
        VLOG_WITH_FUNC(3) << "All streams are ready";
        break;
      }
      VLOG_WITH_FUNC(3) << num_not_ready << " streams need data, continue fetching";
      RETURN_NOT_OK(fetch_func_());
    }
    for (auto it = read_streams_.begin(); it != read_streams_.end(); ++it) {
      if (it->FetchStatus() == StreamFetchStatus::kHasLocalData) {
        read_queue_.push(&*it);
      }
    }
    started_ = true;
    VLOG_WITH_FUNC(2) << "Start merging " << read_queue_.size() << " streams";
  } else if (current_stream_) {
    // If the last read stream has more data to read, return it to the read queue.
    while (current_stream_->FetchStatus() == StreamFetchStatus::kNeedsFetch) {
      VLOG_WITH_FUNC(2) << "Current stream needs data to be returned to the read queue";
      RETURN_NOT_OK(fetch_func_());
    }
    if (current_stream_->FetchStatus() == StreamFetchStatus::kHasLocalData) {
      read_queue_.push(current_stream_);
    }
    current_stream_ = nullptr;
  }

  if (read_queue_.empty()) {
    VLOG_WITH_FUNC(2) << "Merging is done";
    return nullptr;
  }

  // Take first element out of the queue. After reading it may have different priority when
  // entering the queue again
  current_stream_ = read_queue_.top();
  VLOG_WITH_FUNC(4) << "Reading row's order: " << comp_.get_order_fn_(current_stream_);
  read_queue_.pop();
  return current_stream_;
}

//--------------------------------------------------------------------------------------------------

PgDocResponse::PgDocResponse(PerformFuture&& future, const MetricInfo& info)
    : holder_(std::in_place_type<FutureInfo>, std::move(future), info) {}

PgDocResponse::PgDocResponse(ProviderPtr provider)
    : holder_(std::move(provider)) {}

bool PgDocResponse::Valid() const {
  return std::holds_alternative<FutureInfo>(holder_)
      ? std::get<FutureInfo>(holder_).future.Valid()
      : static_cast<bool>(std::get<ProviderPtr>(holder_));
}

Result<PgDocResponse::Data> PgDocResponse::Get(PgSession& session) {
  DCHECK(Valid());
  if (std::holds_alternative<FutureInfo>(holder_)) {
    return GetResponse(std::get<FutureInfo>(holder_), session);
  }

  // Detach provider pointer after first usage to make PgDocResponse::Valid return false.
  ProviderPtr provider;
  std::get<ProviderPtr>(holder_).swap(provider);
  return provider->Get();
}

//--------------------------------------------------------------------------------------------------

PgDocOp::PgDocOp(const PgSession::ScopedRefPtr& pg_session, PgTable* table, const Sender& sender)
    : pg_session_(pg_session), table_(*table), sender_(sender) {}

Status PgDocOp::ExecuteInit(const YbcPgExecParameters *exec_params) {
  end_of_data_ = false;
  if (exec_params) {
    exec_params_ = *exec_params;
  }
  return Status::OK();
}

const YbcPgExecParameters& PgDocOp::ExecParameters() const {
  return exec_params_;
}

Result<RequestSent> PgDocOp::Execute(ForceNonBufferable force_non_bufferable) {
  // As of 09/25/2018, DocDB doesn't cache or keep any execution state for a statement, so we
  // have to call query execution every time.
  // - Normal SQL convention: Exec, Fetch, Fetch, ...
  // - Our SQL convention: Exec & Fetch, Exec & Fetch, ...
  // This refers to the sequence of operations between this layer and the underlying tablet
  // server / DocDB layer, not to the sequence of operations between the PostgreSQL layer and this
  // layer.
  RETURN_NOT_OK(SendRequest(force_non_bufferable));
  return RequestSent(response_.Valid());
}

Status PgDocOp::FetchMoreResults() {
  // If the execution has error, return without reading any rows.
  RETURN_NOT_OK(exec_status_);

  if (end_of_data_) {
    return Status::OK();
  }

  // Send request now in case prefetching was suppressed.
  if (suppress_next_result_prefetching_ && !response_.Valid()) {
    RETURN_NOT_OK(SendRequest());
  }

  // This may block until results are available
  const auto result_data = VERIFY_RESULT(response_.Get(*pg_session_));
  RETURN_NOT_OK(ProcessResponse(result_data));

  // In case ProcessResponse doesn't fail with an error
  // it should return non empty rows, copy paging info from the responses into requests
  // and set end_of_data_.
  // Prefetch next portion of data if needed.
  if (!(end_of_data_ || suppress_next_result_prefetching_)) {
    RETURN_NOT_OK(SendRequest());
  }

  return Status::OK();
}

PgDocResultStream& PgDocOp::ResultStream() {
  static CachedPgDocResultStream dummy_stream({});
  return result_stream_ ? *result_stream_ : dummy_stream;
}

Result<int32_t> PgDocOp::GetRowsAffectedCount() const {
  RETURN_NOT_OK(exec_status_);
  DCHECK(end_of_data_);
  return rows_affected_count_;
}

void PgDocOp::ResetResultStream() {
  if (result_stream_) {
    result_stream_->ResetOps(pgsql_ops_);
  }
}

void PgDocOp::MoveInactiveOpsOutside() {
  const auto inactive_op_begin = std::partition(
      pgsql_ops_.begin(), pgsql_ops_.end(),
      [](const auto& op) { return op->is_active(); });
  active_op_count_ = inactive_op_begin - pgsql_ops_.begin();
}

Status PgDocOp::SendRequest(ForceNonBufferable force_non_bufferable) {
  DCHECK(exec_status_.ok());
  DCHECK(!response_.Valid());
  exec_status_ = SendRequestImpl(force_non_bufferable);
  return exec_status_;
}

Status PgDocOp::SendRequestImpl(ForceNonBufferable force_non_bufferable) {
  // Populate collected information into protobuf requests before sending to DocDB.
  RETURN_NOT_OK(CreateRequests());

  // Exit if there's nothing to send
  if (active_op_count_ == 0) {
    return Status::OK();
  }

  // Send at most "parallelism_level_" number of requests at one time.
  size_t send_count = std::min(parallelism_level_, active_op_count_);

  uint64_t max_size = FLAGS_rpc_max_message_size * FLAGS_max_buffer_size_to_rpc_limit_ratio
                      / send_count;

  for (const auto& op : pgsql_ops_) {
    if (op->is_active() && op->is_read()) {
      auto& read_op = down_cast<PgsqlReadOp&>(*op);
      auto req_size_limit = read_op.read_request().size_limit();
      if (req_size_limit > 0) {
        VLOG(2) << "Capping read op at size limit: " << max_size
                << " (from " << req_size_limit << ")";
      }

      // Cap the size limit if the size limit is unset or exceeds the maximum size.
      if (req_size_limit > max_size || req_size_limit == 0) {
            read_op.read_request().set_size_limit(max_size);
      }
    }
  }

  VLOG(1) << "Number of operations to send: " << send_count;
  response_ = VERIFY_RESULT(sender_(
      pg_session_.get(), pgsql_ops_.data(), send_count, *table_,
      HybridTime::FromPB(GetInTxnLimitHt()), force_non_bufferable, IsForWritePgDoc(IsWrite())));
  if (!result_stream_) {
    // Default PgDocResultStream
    result_stream_ = std::make_unique<ParallelPgDocResultStream>(
        [this]() { return this->FetchMoreResults(); }, pgsql_ops_);
  }
  return Status::OK();
}

void PgDocOp::RecordRequestMetrics() {
  auto& metrics = pg_session_->metrics();
  for (const auto& op : pgsql_ops_) {
    const auto* response = op->response();
    if (!(op->is_active() && response && response->has_metrics())) {
      continue;
    }
    const auto& response_metrics = response->metrics();

    // Record the number of DocDB rows read.
    // Index Scans on secondary indexes in colocated tables need special handling: the target
    // table for such ops is the secondary index, however the scanned_table_rows field returns
    // the number of main table rows scanned scanned. Hence, if a request has both table and index
    // rows_scanned set, then we do not resolve the target table type. In all other cases, the
    // index_rows_scanned field is not populated and the table type can be resolved to figure out
    // the kind of rows scanned.
    if (!response_metrics.has_scanned_table_rows()) {
      continue;
    }

    if (table_->IsColocated() && response_metrics.has_scanned_index_rows()) {
      metrics.RecordStorageRowsRead(TableType::USER, response_metrics.scanned_table_rows());
      metrics.RecordStorageRowsRead(TableType::INDEX, response_metrics.scanned_index_rows());
    } else {
      metrics.RecordStorageRowsRead(
          ResolveRelationType(*op, *table_), response_metrics.scanned_table_rows());
    }
  }
}

Status PgDocOp::ProcessResponse(const Result<PgDocResponse::Data>& response) {
  VLOG(1) << __PRETTY_FUNCTION__ << ": Received response for request " << this;
  // Check operation status.
  DCHECK(exec_status_.ok());

  RecordRequestMetrics();

  exec_status_ = ProcessResponseImpl(response);
  return exec_status_;
}

Status PgDocOp::ProcessResponseImpl(const Result<PgDocResponse::Data>& response) {
  if (!response.ok()) {
    return response.status();
  }
  const auto& data = *response;
  RETURN_NOT_OK(ProcessCallResponse(*data.response));

  if (data.used_in_txn_limit) {
    GetInTxnLimitHt() = data.used_in_txn_limit.ToUint64();
  }
  return CompleteProcessResponse();
}

Status PgDocOp::ProcessCallResponse(const rpc::CallResponse& response) {
  // Process data coming from tablet server.

  rows_affected_count_ = 0;
  for (auto& op : pgsql_ops_) {
    if (!op->is_active()) {
      break;
    }
    auto* op_response = op->response();
    if (!op_response) {
      continue;
    }

    if (op_response->has_partition_list_version()) {
      table_->SetLatestKnownPartitionListVersion(op_response->partition_list_version());
    }

    rows_affected_count_ += op_response->rows_affected_count();
    if (!op_response->has_rows_data_sidecar()) {
      continue;
    }

    // A single batch of requests almost always is directed to fetch data from a single tablet.
    // However, when tablets split, data can be sharded/distributed across multiple tablets.
    // Due to automatic tablet splitting, there can exist scenarios where a pgsql_operation prepares
    // a request for a single tablet before the split occurs and then a tablet can be split to
    // multiple tablets. Hence, a single pgsql_op would potentially end up fetching data from
    // multiple tablets.
    //
    // For example consider a query select * from table where i=1 order by j;
    // where there exists a secondary index table_i_j_idx (i HASH, j ASC). Since, there is an
    // ordering constraint, the ybctids are ordered on j;
    //     ybctid[partition 1] contains (ybctid_1, ybctid_3, ybctid_4)
    //     ybctid[partition 2] contains (ybctid_2, ybctid_6)
    //     ybctid[partition 3] contains (ybctid_5, ybctid_7)
    //
    // say partition 1 splits into partition 1.1 and partition 1.2
    // ybctid[partition 1.1] contains (ybctid_1, ybctid_4) -> pgsql_op for partition 1
    // ybctid[partition 1.2] contains (ybctid_3) -> pgsql_op partition 1 with paging state
    //
    // In order to match the ordering constraints between the request and the responses, we
    // obtain the orders of requests executed in each partition and send it along with the responses
    // so that pg_gate can send responses to the postgres layer in the correct order.

    auto rows_data = VERIFY_RESULT(response.GetSidecarHolder(op_response->rows_data_sidecar()));
    RETURN_NOT_OK(ResultStream().EmplaceOpDocResult(op, std::move(rows_data), *op_response));
  }

  return Status::OK();
}

uint64_t& PgDocOp::GetInTxnLimitHt() {
  return !IsWrite() && exec_params_.stmt_in_txn_limit_ht_for_reads
      ? *exec_params_.stmt_in_txn_limit_ht_for_reads
      : in_txn_limit_ht_;
}

Status PgDocOp::CreateRequests() {
  if (!request_population_completed_) {
    if (VERIFY_RESULT(DoCreateRequests())) {
      request_population_completed_ = true;
    }
  }
  return CompleteRequests();
}

Result<bool> PgDocOp::PopulateByYbctidOps(const YbctidGenerator& generator, KeepOrder keep_order) {
  RETURN_NOT_OK(DoPopulateByYbctidOps(generator, keep_order));
  request_population_completed_ = true;
  if (active_op_count_ > 0) {
    RETURN_NOT_OK(CompleteRequests());
    return true;
  }
  return false;
}

Status PgDocOp::CompleteRequests() {
  for (const auto& op : pgsql_ops_) {
    RETURN_NOT_OK(op->InitPartitionKey(*table_));
  }
  return Status::OK();
}

Result<PgDocResponse> PgDocOp::DefaultSender(
    PgSession* session, const PgsqlOpPtr* ops, size_t ops_count, const PgTableDesc& table,
    HybridTime in_txn_limit, ForceNonBufferable force_non_bufferable, IsForWritePgDoc is_write) {
  const PgDocResponse::MetricInfo metrics{ResolveRelationType(**ops, table), is_write};
  auto result = PgDocResponse{VERIFY_RESULT(session->RunAsync(
      ops, ops_count, table, in_txn_limit, force_non_bufferable)), metrics};
  if (!result.Valid()) {
    RETURN_NOT_OK(UpdateMetricOnRequestBuffering(metrics, session->metrics()));
  }
  return result;
}

//-------------------------------------------------------------------------------------------------

PgDocReadOp::PgDocReadOp(const PgSession::ScopedRefPtr& pg_session,
                         PgTable* table,
                         PgsqlReadOpPtr read_op)
    : PgDocOp(pg_session, table), read_op_(std::move(read_op)) {}

PgDocReadOp::PgDocReadOp(const PgSession::ScopedRefPtr& pg_session,
                         PgTable* table,
                         PgsqlReadOpPtr read_op,
                         const Sender& sender)
    : PgDocOp(pg_session, table, sender), read_op_(std::move(read_op)) {}

Status PgDocReadOp::ExecuteInit(const YbcPgExecParameters* exec_params) {
  RSTATUS_DCHECK(
      pgsql_ops_.empty() || !exec_params,
      IllegalState, "Exec params can't be changed for already created operations");
  RETURN_NOT_OK(PgDocOp::ExecuteInit(exec_params));
  if (!VERIFY_RESULT(SetScanBounds())) {
    end_of_data_ = true;
    return Status::OK();
  }

  read_op_->read_request().set_return_paging_state(true);
  RETURN_NOT_OK(SetRequestPrefetchLimit());
  SetBackfillSpec();
  SetRowMark();
  SetReadTimeForBackfill();
  return Status::OK();
}

Result<bool> PgDocReadOp::SetScanBounds() {
  if (table_->schema().num_hash_key_columns() > 0) {
    // TODO: figure out if we can clarify bounds of a hash table scan
    return true;
  }
  std::vector<dockv::KeyEntryValue> lower_range_components, upper_range_components;
  auto& request = read_op_->read_request();
  RETURN_NOT_OK(client::GetRangePartitionBounds(
      table_->schema(), request, &lower_range_components, &upper_range_components));
  if (lower_range_components.empty() && upper_range_components.empty()) {
    return true;
  }
  auto lower_bound = dockv::DocKey(
      table_->schema(), std::move(lower_range_components)).Encode().ToStringBuffer();
  auto upper_bound = dockv::DocKey(
      table_->schema(), std::move(upper_range_components)).Encode().ToStringBuffer();
  VLOG_WITH_FUNC(2) << "Lower bound: " << Slice(lower_bound).ToDebugHexString()
                    << ", upper bound: " << Slice(upper_bound).ToDebugHexString();
  return table_->SetScanBoundary(&request, lower_bound, true, upper_bound, false);
}

bool CouldBeExecutedInParallel(const LWPgsqlReadRequestPB& req) {
  if (req.index_request().has_vector_idx_options()) {
    // Executed in parallel on PgClient
    return false;
  }
  // At this time ordered scan requires tablet scan order, so they have to be done sequentially
  return !req.has_is_forward_scan();
}

Result<bool> PgDocReadOp::DoCreateRequests() {
  // All information from the SQL request has been collected and setup. This code populates
  // Protobuf requests before sending them to DocDB. For performance reasons, requests are
  // constructed differently for different statements.
  const auto& req = read_op_->read_request();
  // Use partition column values to filter out partitions without possible matches.
  // If there is a query with conditions on multiple partition columns, like
  // - SELECT * FROM sql_table WHERE hash_c1 IN (1, 2, 3) AND hash_c2 IN (4, 5, 6);
  // the function creates multiple requests for possible hash permutations / keys.
  // The requests are also configured to run in parallel.
  if (!req.partition_column_values().empty()) {
    return PopulateNextHashPermutationOps();

  // There is no key values to filter out partitions, send requests to all of them.
  // Requests pushing down aggregates and/or filter expression tend to do more work on DocDB side,
  // so it takes longer to return responses, and Postgres side has generally less work to do.
  // Hence we optimize by sending multiple parallel requests to the tablets, allowing their
  // simultaneous processing.
  // Effect may be less than expected if the nodes are already heavily loaded and CPU consumption
  // is high, or selectivity of the filter is low.
  // Other concern than Postgres being a bottleneck, is an index scan over a range partitioned
  // table. Such scan is able to return rows ordered, and that order may be relied upon by upper
  // plan nodes, but parallel execution messes the order up. Here where we are preparing the
  // requests, we do not know if the query relies on the row order, we even may not be able to tell,
  // if we are doing sequential or primary key scan, hence we should not parallelize non-aggregate
  // scans on range partitioned tables.
  // TODO(GHI 13737): as explained above, explicitly indicate, if operation should return ordered
  // results.
  } else if (CouldBeExecutedInParallel(req)) {
    return PopulateParallelSelectOps();
  } else {
    // No optimization.
    if (exec_params_.partition_key != nullptr) {
      if (!VERIFY_RESULT(SetScanPartitionBoundary())) {
        // Target partition boundaries do not intersect with request boundaries, no results
        return false;
      }
    }
    ClonePgsqlOps(1);
    auto& read_op = GetReadOp(0);
    read_op.set_active(true);
    active_op_count_ = 1;
    if (req.has_ybctid_column_value()) {
      const Slice& ybctid = req.ybctid_column_value().value().binary_value();
      const size_t partition = VERIFY_RESULT(table_->FindPartitionIndex(ybctid));
      return SetLowerUpperBound(&read_op.read_request(), partition);
    }
    return true;
  }
}

Status PgDocReadOp::DoPopulateByYbctidOps(const YbctidGenerator& generator, KeepOrder keep_order) {
  // NOTE on a typical process.
  // 1- Statement:
  //    SELECT xxx FROM <table> WHERE ybctid IN (SELECT ybctid FROM INDEX);
  //
  // 2- Select 1024 ybctids (prefetch limit) from INDEX.
  //
  // 3- ONLY ONE TIME: Create a batch of operators, one per partition.
  //    * Each operator has a clone requests from template_op_.
  //    * We will reuse the created operators & requests for the future batches of 1024 ybctids.
  //    * Certain fields in the protobuf requests MUST BE RESET for each batches.
  //
  // 4- Assign the selected 1024 ybctids to the batch of operators.
  //
  // 5- Send requests to tablet servers to read data from <tab> associated with ybctid values.
  //
  // 6- Repeat step 2 thru 5 for the next batch of 1024 ybctids till done.
  InitializeYbctidOperators();
  end_of_data_ = false;
  VLOG(1) << "Row order " << (keep_order ? "is" : "is not") << " important";
  if (keep_order) {
    if (!result_stream_) {
      result_stream_ = std::make_unique<MergingPgDocResultStream<int64_t>>(
          [this]() { return this->FetchMoreResults(); },
          pgsql_ops_,
          [](PgsqlResultStream* stream) { return stream->NextRowOrder(); });
      VLOG(3) << "Created MergingPgDocResultStream";
    }
  }
  while (true) {
    const auto ybctid = generator.next();
    if (ybctid.empty()) {
      break;
    }
    // Find partition. The partition index is the boundary order minus 1.
    // - For hash partitioning, we use hashcode to find the right index.
    // - For range partitioning, we pass partition key to seek the index.
    const auto partition = VERIFY_RESULT(table_->FindPartitionIndex(ybctid));

    // Assign ybctids to operators.
    auto& read_op = GetReadOp(partition);
    auto& read_req = read_op.read_request();

    // Check bounds, if set.
    // We also ensure that the bounds are valid ybctids. Hash partitioned relations use hash codes
    // as partitioning keys, they are not comparable to ybctids. Other partitioning types use keys
    // comparable with ybctids.
    if (read_req.has_lower_bound()) {
      const auto& lower_bound = read_req.lower_bound();
      if (!dockv::PartitionSchema::IsValidHashPartitionKeyBound(lower_bound.key().ToBuffer()) &&
          (lower_bound.is_inclusive() ? ybctid < lower_bound.key() : ybctid <= lower_bound.key())) {
        continue;
      }
    }
    if (read_req.has_upper_bound()) {
      const auto& upper_bound = read_req.upper_bound();
      if (!dockv::PartitionSchema::IsValidHashPartitionKeyBound(upper_bound.key().ToBuffer()) &&
          (upper_bound.is_inclusive() ? ybctid > upper_bound.key() : ybctid >= upper_bound.key())) {
        continue;
      }
    }

    // Append ybctid and its order to batch_arguments.
    // The "ybctid" values are returned in the same order as the row in the IndexTable. To keep
    // track of this order, each argument is assigned an order-number.
    auto* batch_arg = read_req.add_batch_arguments();
    if (keep_order) {
      batch_arg->set_order(batch_row_ordering_counter_++);
    }
    auto* arg_value = batch_arg->mutable_ybctid()->mutable_value();
    arg_value->dup_binary_value(ybctid);

    // We must set "ybctid_column_value" in the request for the sake of rolling upgrades.
    // Servers before 2.15 will read only "ybctid_column_value" as they are not aware
    // of ybctid-batching.
    if (!read_req.has_ybctid_column_value() ||
        arg_value->binary_value() < read_req.ybctid_column_value().value().binary_value()) {
      read_req.mutable_ybctid_column_value()->mutable_value()
          ->ref_binary_value(arg_value->binary_value());
    }

    // For every read operation set partition boundary. In case a tablet is split between
    // preparing requests and executing them, DocDB will return a paging state for pggate to
    // continue till the end of current tablet is reached.
    if (VERIFY_RESULT(SetLowerUpperBound(&read_req, partition))) {
      read_op.set_active(true);
    }
  }

  // Done creating request, but not all partition or operator has arguments (inactive).
  MoveInactiveOpsOutside();

  return Status::OK();
}

void PgDocReadOp::InitializeYbctidOperators() {
  if (!pgsql_op_arena_) {
    // First batch, create arena for operations
    DCHECK(pgsql_ops_.empty());
    pgsql_op_arena_ = SharedArena();
  } else if (active_op_count_ == 0) {
    // All past operations are done, can perform full reset to release memory
    pgsql_ops_.clear();
    pgsql_op_arena_->Reset(ResetMode::kKeepLast);
  }
  ResetInactivePgsqlOps();
  ClonePgsqlOps(table_->GetPartitionListSize());
}

LWPgsqlReadRequestPB* PgDocReadOp::PrepareReadReq() {
  // Set the index at the start of inactive operators.
  auto op_count = pgsql_ops_.size();
  auto op_index = active_op_count_;
  if (op_index >= op_count)
    return nullptr;

  pgsql_ops_[op_index]->set_active(true);
  auto& req = GetReadReq(op_index);
  active_op_count_ = ++op_index;
  return &req;
}

bool PgDocReadOp::HasNextPermutation() const {
  return next_permutation_idx_ < total_permutation_count_;
}

bool PgDocReadOp::GetNextPermutation(std::vector<const LWPgsqlExpressionPB *>* permutation) {
  if (!HasNextPermutation()) {
    return false;
  }

  auto pos = next_permutation_idx_++;
  for (const auto& partition_exprs_it : partition_exprs_) {
    if (partition_exprs_it.empty()) {
      continue;
    }

    auto sel_idx = pos % partition_exprs_it.size();
    pos /= partition_exprs_it.size();
    auto expr = partition_exprs_it[sel_idx];
    permutation->push_back(expr);
  }
  return true;
}
void PgDocReadOp::BindExprsRegular(
    LWPgsqlReadRequestPB* read_req,
    const std::vector<const LWPgsqlExpressionPB*>& hashed_values,
    const std::vector<const LWPgsqlExpressionPB*>& range_values) {
  DCHECK(!IsHashBatchingEnabled());
  size_t index = 0;
  // Bind all hash column values.
  for (auto& partition_column : *read_req->mutable_partition_column_values()) {
    partition_column = *hashed_values[index++];
  }

  // Deal with any range columns that are in this tuple IN.
  // Create an equality condition for each column
  for (auto range_idx : permutation_range_column_indexes_) {
    auto* pgexpr = range_values[range_idx - table_->num_hash_key_columns()];
    DCHECK(pgexpr != nullptr);

    read_req->mutable_condition_expr()->mutable_condition()->set_op(QL_OP_AND);
    auto* op = read_req->mutable_condition_expr()->mutable_condition()->add_operands();
    auto* pgcond = op->mutable_condition();
    pgcond->set_op(QL_OP_EQUAL);
    pgcond->add_operands()->set_column_id(table_.ColumnForIndex(range_idx).id());
    *pgcond->add_operands() = *pgexpr;
  }
}

void PgDocReadOp::BindExprsToBatch(
    const std::vector<const LWPgsqlExpressionPB*>& hashed_values,
    const std::vector<const LWPgsqlExpressionPB*>& range_values) {
  auto partition_key = table_->partition_schema().EncodePgsqlHash(hashed_values);
  DCHECK(partition_key.ok());
  auto partition = client::FindPartitionStartIndex(table_->GetPartitionList(), *partition_key);

  if (hash_in_conds_.empty()) {
    hash_in_conds_.resize(table_->GetPartitionListSize(), nullptr);
  }

  // Identify the vector to which the key should be added.
  if (!hash_in_conds_[partition]) {
    hash_in_conds_[partition] = hash_key_arena_->NewArenaObject<LWPgsqlExpressionPB>();
    PrepareInitialHashConditionList(partition);
  }

  DCHECK(hash_in_conds_[partition]->has_condition());
  auto* op = hash_in_conds_[partition]->mutable_condition()->mutable_operands();
  auto* rhs_values_list = op->back().mutable_value()->mutable_list_value();

  // Add new hash keys to the end of the array in the format (yb_hash_code, hk1, hk2 .... hkn)
  auto* tup_elements = rhs_values_list->add_elems()->mutable_tuple_value();
  auto* new_elem = tup_elements->add_elems();
  new_elem->set_int32_value(table_->partition_schema().DecodeMultiColumnHashValue(*partition_key));
  for (auto elem : hashed_values) {
    auto new_elem = tup_elements->add_elems();
    if (elem->has_value()) {
        DCHECK(elem->has_value());
        *new_elem = elem->value();
    }
  }

  for (auto range_idx : permutation_range_column_indexes_) {
    auto* elem = range_values[range_idx - table_->num_hash_key_columns()];
    DCHECK(elem != nullptr);
    if (elem->has_value()) {
      auto* new_elem = tup_elements->add_elems();
      *new_elem = elem->value();
    }
  }
}

bool PgDocReadOp::HasNextBatch() {
  while (next_batch_partition_ < hash_in_conds_.size() &&
         hash_in_conds_[next_batch_partition_] == nullptr) {
    next_batch_partition_++;
  }
  return next_batch_partition_ < hash_in_conds_.size();
}

static bool IsHashInCondition(const LWPgsqlExpressionPB& cond) {
  if (cond.has_condition() && cond.condition().op() == QL_OP_IN) {
    auto first_elem = cond.condition().operands().begin();
    return first_elem->has_tuple() &&
           first_elem->tuple().elems().begin()->has_column_id() &&
           first_elem->tuple().elems().begin()->column_id() == kYbHashCodeColId;
  }
  return false;
}

Result<bool> PgDocReadOp::BindNextBatchToRequest(LWPgsqlReadRequestPB* read_req) {
  DCHECK(read_req != nullptr);

  if (!HasNextBatch()) {
    return true;
  }

  auto current_partition = next_batch_partition_;

  auto* cond = hash_in_conds_[current_partition];

  LWPgsqlExpressionPB* cond_bind_expr = nullptr;

  if (!read_req->has_condition_expr()) {
    read_req->mutable_condition_expr()->mutable_condition()->set_op(QL_OP_AND);
    cond_bind_expr = read_req->mutable_condition_expr()->mutable_condition()->add_operands();
  } else {
    bool hash_in_found = false;
    for (auto& cond_it :
         *read_req->mutable_condition_expr()->mutable_condition()->mutable_operands()) {
      // See if it already has a hash IN condition from a previous binding and clear it.
      if (IsHashInCondition(cond_it)) {
        cond_it.clear_condition();
        cond_bind_expr = &cond_it;
        hash_in_found = true;
        break;
      }
    }

    if (!hash_in_found) {
      cond_bind_expr = read_req->mutable_condition_expr()->mutable_condition()->add_operands();
    }
  }

  cond_bind_expr->CopyFrom(*cond);
  read_req->clear_partition_column_values();

  VERIFY_RESULT(SetLowerUpperBound(read_req, current_partition));

  next_batch_partition_++;
  return false;
}

bool PgDocReadOp::IsHashBatchingEnabled() {
  if (PREDICT_FALSE(!is_hash_batched_.has_value())) {
    is_hash_batched_ = pg_session_->IsHashBatchingEnabled() && total_permutation_count_ > 1;
  }
  return *is_hash_batched_;
}

bool PgDocReadOp::IsBatchFlushRequired() const {
  return !hash_in_conds_.empty() &&
      next_permutation_idx_ > 0 &&
      exec_params_.work_mem >= 0 &&
      hash_key_arena_->UsedBytes() > (implicit_cast<size_t>(exec_params_.work_mem) * 1024);
}

void PgDocReadOp::ResetHashBatches() {
  hash_key_arena_->Reset(ResetMode::kKeepFirst);
  hash_in_conds_.clear();
  next_batch_partition_ = 0;
}

void PgDocReadOp::BindPermutation(const std::vector<const LWPgsqlExpressionPB*>& exprs,
                                  LWPgsqlReadRequestPB* read_req) {
  const size_t hash_column_count = table_->num_hash_key_columns();
  std::vector<const LWPgsqlExpressionPB*> hash_exprs(hash_column_count,
    nullptr);
  std::vector<const LWPgsqlExpressionPB*> range_exprs(table_->num_range_key_columns(), nullptr);
  auto cond_iter = read_op_->read_request().partition_column_values().begin();
  size_t index = 0;
  for (auto* expr : exprs) {
    if (hash_exprs[index]) {
      continue;
    }

    if (expr->value().has_tuple_value()) {
      const auto& lhs_key_cols = *cond_iter->condition().operands().begin();
      auto val_it = expr->value().tuple_value().elems().begin();

      // Bind all the values for this tuple.
      for (const auto& lhs_elem : lhs_key_cols.tuple().elems()) {
        // Get the value for this column in the tuple.
        size_t tup_c_idx = lhs_elem.column_id() - table_->schema().first_column_id();
        auto* pgexpr =
            hash_key_arena_->NewArenaObject<LWPgsqlExpressionPB>();
        *pgexpr->mutable_value() = *val_it;

        if (tup_c_idx < hash_column_count) {
          hash_exprs[tup_c_idx] = pgexpr;
        } else {
          range_exprs[tup_c_idx - hash_column_count] = pgexpr;
        }
        val_it++;
      }
    } else {
      hash_exprs[index] = expr;
    }

    cond_iter++;
    index++;
  }

  if (IsHashBatchingEnabled()) {
    BindExprsToBatch(hash_exprs, range_exprs);
  } else {
    BindExprsRegular(read_req, hash_exprs, range_exprs);
  }
}

Result<bool> PgDocReadOp::PopulateNextHashPermutationOps() {
  InitializeHashPermutationStates();

  std::vector<const LWPgsqlExpressionPB*> current_permutation;
  current_permutation.reserve(partition_exprs_.size());
  // Bind new hash permutations.

  while (HasNextPermutation()) {
    LWPgsqlReadRequestPB* read_req = nullptr;
    if (!IsHashBatchingEnabled() &&
        (read_req = PrepareReadReq()) == nullptr) {
      return false;
    }
    current_permutation.clear();
    GetNextPermutation(&current_permutation);

    // read_req is nullptr if batching is on as we don't want to
    // bind the permutation to a request yet. We instead want to bind
    // it to one of the batches we are building up.
    BindPermutation(current_permutation, read_req);

    // If we run out of memory and have to flush our currently formed
    // permutations in batched mode, let us do so.
    if (IsHashBatchingEnabled() &&
        IsBatchFlushRequired() &&
        !VERIFY_RESULT(BindBatchesToRequests())) {
      // We failed to bind all batches to a request, so we return false.
      return false;
    }
  }

  // If batching is enabled, we have built up all our batches and now
  // we can bind each batch to a request.
  if (IsHashBatchingEnabled()) {
    // If we didn't have enough available requests to put our batches into
    // or we're not done processing all our permutations, return false, signalling
    // that we are not done creating requests.
    if (!VERIFY_RESULT(BindBatchesToRequests())) {
      return false;
    }
  }

  MoveInactiveOpsOutside();
  return true;
}

Result<bool> PgDocReadOp::BindBatchesToRequests() {
  DCHECK(IsHashBatchingEnabled());
  while (HasNextBatch()) {
    LWPgsqlReadRequestPB* read_req = PrepareReadReq();
    if (!read_req) {
      return false;
    }
    VERIFY_RESULT(BindNextBatchToRequest(read_req));
  }

  // Free resources attached to hash batches if we're done binding.
  ResetHashBatches();
  return true;
}

// Add the list of column ids as a tuple. Subsequently every tuple of hash key columns that are
// added will be in the same order as the column ids.
void PgDocReadOp::PrepareInitialHashConditionList(size_t partition) {
  DCHECK(hash_in_conds_[partition] != nullptr);
  hash_in_conds_[partition]->mutable_condition()->set_op(QL_OP_IN);
  auto* add_targets =
    hash_in_conds_[partition]->mutable_condition()->add_operands()->mutable_tuple();

  // YB columns adheres to the following order --> hash key columns and then range key columns
  // We also follow the same order. However, each hash key tuple also contains the yb_hash_code
  // as the first element as docDB scans require that. Hence, in order to match that column id
  // we add the column id of yb_hash_code. However, yb_hash_code has no column id. And hence we
  // assign kFirstColumnIdRep -1 as the column id for yb_hash_code.
  auto* yb_hash_code = add_targets->add_elems();
  yb_hash_code->set_column_id(kYbHashCodeColId);
  for (size_t i = 0; i < table_->num_hash_key_columns(); i++) {
    auto& col = table_.ColumnForIndex(i);
    auto* hash_column = add_targets->add_elems();
    hash_column->set_column_id(col.id());
  }
  for (auto c_idx : permutation_range_column_indexes_) {
    auto& col = table_.ColumnForIndex(c_idx);
    auto* range_column = add_targets->add_elems();
    range_column->set_column_id(col.id());
  }

  // RHS of IN condition
  hash_in_conds_[partition]->mutable_condition()->add_operands();
}

// Collect hash expressions to prepare for generating permutations.
void PgDocReadOp::InitializeHashPermutationStates() {
  // Return if state variables were initialized.
  if (!partition_exprs_.empty()) {
    // Reset the protobuf request before reusing the operators.
    ResetInactivePgsqlOps();
    return;
  }

  // Initialize partition_exprs_.
  // Reorganize the input arguments from Postgres to prepare for permutation generation.
  auto hash_column_count = table_->num_hash_key_columns();
  partition_exprs_.resize(hash_column_count);
  auto c_idx = 0;
  for (const auto& col_expr : read_op_->read_request().partition_column_values()) {
    if (col_expr.has_condition()) {
      auto it = col_expr.condition().operands().begin();

      // Populate permutation_range_column_indexes_.
      if (it->has_tuple()) {
        for (const auto& lhs_elem : it->tuple().elems()) {
          // Get the value for this column in the tuple.
          size_t tup_c_idx = lhs_elem.column_id() - table_->schema().first_column_id();

          if (tup_c_idx >= hash_column_count) {
            permutation_range_column_indexes_.emplace_back(tup_c_idx);
          }
        }
      }

      ++it;
      for (const auto& expr : it->condition().operands()) {
        partition_exprs_[c_idx].push_back(&expr);
      }
    } else {
      partition_exprs_[c_idx].push_back(&col_expr);
    }
    ++c_idx;
  }

  // Make sure permutation_range_column_indexes_ is ordered.
  std::sort(permutation_range_column_indexes_.begin(), permutation_range_column_indexes_.end());

  // Calculate the total number of permutations to be generated.
  total_permutation_count_ = 1;
  for (const auto& exprs : partition_exprs_) {
    // If exprs is empty that means this column is part of a
    // tuple condition that has already been accounted for by a
    // previous column
    if (exprs.size() > 0) {
      total_permutation_count_ *= exprs.size();
    }
  }

  // Create operators, one operation per partition, up to FLAGS_ysql_request_limit.
  //
  // TODO(neil) The control variable "ysql_request_limit" should be applied to ALL statements, but
  // at the moment, the number of operators never exceeds the number of tablets except for hash
  // permutation operation, so the work on this GFLAG can be done when it is necessary.
  auto max_op_count =
      std::min(total_permutation_count_,
               implicit_cast<size_t>(FLAGS_ysql_request_limit));

  if (IsHashBatchingEnabled()) {
    max_op_count = std::min(max_op_count, table_->GetPartitionListSize());
  }

  ClonePgsqlOps(max_op_count);

  // Clear the original partition expressions as it will be replaced with hash permutations.
  for (size_t op_index = 0; op_index < max_op_count; ++op_index) {
    auto& read_request = GetReadReq(op_index);
    read_request.mutable_partition_column_values()->clear();
    if (IsHashBatchingEnabled()) {
      read_request.clear_hash_code();
      read_request.clear_max_hash_code();
    } else {
      for (size_t i = 0; i < hash_column_count; ++i) {
        read_request.add_partition_column_values();
      }
    }
    pgsql_ops_[op_index]->set_active(false);
  }

  // Initialize counters.
  next_permutation_idx_ = 0;
  active_op_count_ = 0;
  next_batch_partition_ = 0;

  hash_key_arena_ = std::make_unique<ThreadSafeArena>();
}

Result<bool> PgDocReadOp::PopulateParallelSelectOps() {
  SCHECK(read_op_->read_request().partition_column_values().empty(),
         IllegalState,
         "Request with non empty partition_column_values can't be parallelized");
  // Create batch operators, one per partition, to execute in parallel.
  // TODO(tsplit): what if table partition is changed during PgDocReadOp lifecycle before or after
  // the following line?
  ClonePgsqlOps(table_->GetPartitionListSize());
  // Set "parallelism_level_" to control how many operators can be sent at one time.
  //
  // TODO(neil) The calculation for this control variable should be applied to ALL operators, but
  // the following calculation needs to be refined before it can be used for all statements.
  auto parallelism_level = FLAGS_ysql_select_parallelism;
  if (parallelism_level < 0) {
    int tserver_count = VERIFY_RESULT(pg_session_->TabletServerCount(true /* primary_only */));

    // Establish lower and upper bounds on parallelism.
    int kMinParSelParallelism = 1;
    int kMaxParSelParallelism = 16;
    parallelism_level_ =
      std::min(std::max(tserver_count * 2, kMinParSelParallelism), kMaxParSelParallelism);
  } else {
    parallelism_level_ = parallelism_level;
  }

  // Get table partitions
  const auto& partition_keys = table_->GetPartitionList();
  SCHECK_EQ(partition_keys.size(), pgsql_ops_.size(), IllegalState,
            "Number of partitions and number of partition keys are not the same");

  // Create operations, one per partition, and apply partition boundaries to ther requests.
  // Activate operation if partition boundaries intersect with boundaries that may be already set.
  // If boundaries do not intersect the operation won't produce any result.
  for (size_t partition = 0; partition < partition_keys.size(); ++partition) {
    if (VERIFY_RESULT(SetLowerUpperBound(&GetReadReq(partition), partition))) {
      pgsql_ops_[partition]->set_active(true);
      ++active_op_count_;
    }
  }
  // Got some inactive operations, move them away
  if (active_op_count_ < pgsql_ops_.size()) {
    MoveInactiveOpsOutside();
  }

  return true;
}

// When postgres requests to scan a specific partition, set the partition parameter accordingly.
Result<bool> PgDocReadOp::SetScanPartitionBoundary() {
  // Boundary to scan from a given key to the end of its associated tablet.
  // - Lower: The given partition key (inclusive).
  // - Upper: Beginning of next tablet (not inclusive).
  SCHECK(exec_params_.partition_key != nullptr, Uninitialized, "expected non-null partition_key");

  // Seek the tablet of the given key.
  // TODO(tsplit): what if table partition is changed during PgDocReadOp lifecycle before or after
  // the following line?
  const auto& partition_keys = table_->GetPartitionList();
  const auto& partition_key = std::find(partition_keys.begin(),
                                        partition_keys.end(),
                                        a2b_hex(exec_params_.partition_key));
  RSTATUS_DCHECK(
      partition_key != partition_keys.end(), InvalidArgument, "invalid partition key given");

  // Seek upper bound (Beginning of next tablet).
  std::string upper_bound;
  const auto& next_partition_key = std::next(partition_key, 1);
  if (next_partition_key != partition_keys.end()) {
    upper_bound = *next_partition_key;
  }
  return table_->SetScanBoundary(&read_op_->read_request(),
                                 *partition_key,
                                 true /* lower_bound_is_inclusive */,
                                 upper_bound,
                                 false /* upper_bound_is_inclusive */);
}

Status PgDocReadOp::CompleteProcessResponse() {
  // For each read_op, set up its request for the next batch of data or make it in-active.
  bool has_more_data = false;
  auto send_count = std::min(parallelism_level_, active_op_count_);

  for (size_t op_index = 0; op_index < send_count; op_index++) {
    auto& read_op = GetReadOp(op_index);

    // Check for completion.
    bool has_more_arg = false;
    auto& res = *read_op.response();
    auto& req = read_op.read_request();

    // Save the backfill_spec if tablet server wants to return it.
    if (res.is_backfill_batch_done()) {
      out_param_backfill_spec_ = res.backfill_spec().ToBuffer();
    } else if (VERIFY_RESULT(PrepareNextRequest(*table_, &read_op))) {
      has_more_arg = true;
    }

    // Check for batch execution.
    if (res.batch_arg_count() < static_cast<int64_t>(req.batch_arguments().size())) {
      has_more_arg = true;

      // Delete the executed arguments from batch and keep those that haven't been executed.
      for (auto i = res.batch_arg_count(); i-- > 0;) {
        req.mutable_batch_arguments()->pop_front();
      }

      // Due to rolling upgrade reason, we must copy the first batch_arg to the scalar arg.
      FormulateRequestForRollingUpgrade(&req);
    }

    if (has_more_arg) {
      has_more_data = true;
    } else {
      read_op.set_active(false);
    }
  }

  if (has_more_data || send_count < active_op_count_) {
    // Move inactive ops to the end of
    // to make room for new set of arguments.
    MoveInactiveOpsOutside();
    end_of_data_ = false;
  } else {
    // There should be no active op left in queue.
    active_op_count_ = 0;
    end_of_data_ = request_population_completed_;
  }

  if (exec_params_.out_param && has_out_param_backfill_spec()) {
    YbcPgExecOutParamValue value;
    value.bfoutput = out_param_backfill_spec();
    YBCGetPgCallbacks()->WriteExecOutParam(exec_params_.out_param, &value);
  }

  return Status::OK();
}

Status PgDocReadOp::SetRequestPrefetchLimit() {
  // Predict the maximum prefetch-limit using the associated gflags.
  auto& req = read_op_->read_request();

  // Limits: 0 means 'unlimited'.
  uint64_t predicted_row_limit = exec_params_.yb_fetch_row_limit;
  uint64_t predicted_size_limit = exec_params_.yb_fetch_size_limit;

  // Use statement LIMIT(count + offset) if it is smaller than the predicted limit.
  auto row_limit = exec_params_.limit_count + exec_params_.limit_offset;
  suppress_next_result_prefetching_ = true;

  // If in any of these cases we determine that the default row/size based batch size is lower
  // than the actual requested LIMIT, we enable prefetching. If else, we try
  // to only get the required data in one RPC without prefetching.
  if (exec_params_.limit_use_default) {
    row_limit = predicted_row_limit;
    suppress_next_result_prefetching_ = false;
  } else if (predicted_row_limit > 0 && predicted_row_limit < row_limit) {
    row_limit = predicted_row_limit;
    suppress_next_result_prefetching_ = false;
  } else if (predicted_size_limit > 0) {
    // Try to estimate the total data size of a LIMIT'd query
    // Inaccurate in the presence of varlen targets but not possible to fix that without PG stats;
    size_t row_width = 0;
    for (const LWPgsqlExpressionPB& target : req.targets()) {
      // If target is a system column, we it's probably variable size
      // and we don't have the means to estimate its length.
      if (target.has_column_id()) {
        auto column_id = target.column_id();
        if (column_id < 0) {
            // System columns are usually variable length which we are
            // estimating with the size of a Binary DataType for now.
            row_width += GetTypeInfo(DataType::BINARY)->size;
            continue;
        }
        const ColumnSchema &col_schema =
          VERIFY_RESULT(table_->schema().column_by_id(ColumnId(column_id)));

        // This size is usually the computed sizeof() of the serialized datatype.
        // Its computation can be found in yb/common/types.h
        auto size = col_schema.type_info()->size;
        row_width += size;
      }
    }

    // Prefetch if we expect size limit to occur first, so there will
    // be multiple RPCs until row_limit is reached
    if (row_width > 0 && (predicted_size_limit / row_width) < row_limit) {
      suppress_next_result_prefetching_ = false;
    }
  }

  req.set_limit(row_limit);
  req.set_size_limit(predicted_size_limit);

  VLOG(3) << __func__
          << " exec_params_.limit_count=" << exec_params_.limit_count
          << " exec_params_.limit_offset=" << exec_params_.limit_offset
          << " exec_params_.limit_use_default=" << exec_params_.limit_use_default
          << " predicted_row_limit=" << predicted_row_limit
          << " row_limit=" << row_limit
          << (row_limit == 0 ? " (Unlimited)" : "")
          << " size_limit=" << predicted_size_limit
          << (predicted_size_limit == 0 ? " (Unlimited)" : "");
  return Status::OK();
}

void PgDocReadOp::SetRowMark() {
  auto& req = read_op_->read_request();
  const auto row_mark_type = GetRowMarkType(&exec_params_);
  if (IsValidRowMarkType(row_mark_type)) {
    req.set_row_mark_type(row_mark_type);
    req.set_wait_policy(static_cast<yb::WaitPolicy>(exec_params_.docdb_wait_policy));
  } else {
    req.clear_row_mark_type();
  }
}

void PgDocReadOp::SetBackfillSpec() {
  auto& req = read_op_->read_request();

  if (exec_params_.bfinstr) {
    req.dup_backfill_spec(Slice(exec_params_.bfinstr));
  } else {
    req.clear_backfill_spec();
  }
}

void PgDocReadOp::SetReadTimeForBackfill() {
  if (exec_params_.is_index_backfill) {
    read_op_->read_request().set_is_for_backfill(true);
    // TODO: Change to RSTATUS_DCHECK
    DCHECK(exec_params_.backfill_read_time);
    read_op_->set_read_time(ReadHybridTime::FromUint64(exec_params_.backfill_read_time));
  }
}

void PgDocReadOp::ResetInactivePgsqlOps() {
  // Clear the existing ybctids.
  for (auto op_index = active_op_count_; op_index < pgsql_ops_.size(); op_index++) {
    auto& read_req = GetReadReq(op_index);
    read_req.clear_ybctid_column_value();
    read_req.mutable_batch_arguments()->clear();
    read_req.clear_hash_code();
    read_req.clear_max_hash_code();
    read_req.clear_paging_state();
    read_req.clear_lower_bound();
    read_req.clear_upper_bound();
  }
}

Status PgDocReadOp::ResetPgsqlOps() {
  SCHECK_EQ(active_op_count_, 0,
            IllegalState,
            "Can't reset operations when some of them are active");
  pgsql_ops_.clear();
  request_population_completed_ = false;
  return Status::OK();
}

void PgDocReadOp::FormulateRequestForRollingUpgrade(LWPgsqlReadRequestPB *read_req) {
  if (read_req->batch_arguments().empty()) {
    return;
  }

  // Copy first batch-argument to scalar arg because older server does not support batch arguments.
  auto& batch_arg = read_req->mutable_batch_arguments()->front();

  if (batch_arg.has_ybctid()) {
    read_req->ref_ybctid_column_value(batch_arg.mutable_ybctid());
  }

  if (!batch_arg.partition_column_values().empty()) {
    read_req->set_hash_code(batch_arg.hash_code());
    read_req->set_max_hash_code(batch_arg.max_hash_code());
    read_req->ref_partition_column_values(batch_arg.mutable_partition_column_values());
  }
}

PgsqlReadOp& PgDocReadOp::GetReadOp(size_t op_index) {
  return down_cast<PgsqlReadOp&>(*pgsql_ops_[op_index]);
}

LWPgsqlReadRequestPB& PgDocReadOp::GetReadReq(size_t op_index) {
  return GetReadOp(op_index).read_request();
}

Result<bool> PgDocReadOp::SetLowerUpperBound(LWPgsqlReadRequestPB* request, size_t partition) {
  const auto& partition_keys = table_->GetPartitionList();
  const std::string default_upper_bound;
  const auto& upper_bound = (partition < partition_keys.size() - 1)
      ? partition_keys[partition + 1]
      : default_upper_bound;
  return table_->SetScanBoundary(request,
                                 partition_keys[partition],
                                 /* lower_bound_is_inclusive */ true,
                                 upper_bound,
                                 /* upper_bound_is_inclusive */ false);
}

void PgDocReadOp::ClonePgsqlOps(size_t op_count) {
  // Allocate batch operator, one per partition.
  DCHECK_GT(op_count, 0);
  pgsql_ops_.reserve(op_count);
  const auto& arena = pgsql_op_arena_ ? pgsql_op_arena_ : GetSharedArena(read_op_);
  while (pgsql_ops_.size() < op_count) {
    pgsql_ops_.push_back(read_op_->DeepCopy(arena));
    // Initialize as inactive. Turn it on when setup argument for a specific partition.
    pgsql_ops_.back()->set_active(false);
  }
  ResetResultStream();
  // Set parallelism_level_ to maximum possible of operators to be executed at one time.
  parallelism_level_ = pgsql_ops_.size();
}

//--------------------------------------------------------------------------------------------------

PgDocWriteOp::PgDocWriteOp(const PgSession::ScopedRefPtr& pg_session,
                           PgTable* table,
                           PgsqlWriteOpPtr write_op)
    : PgDocOp(pg_session, table), write_op_(std::move(write_op)) {
}

Status PgDocWriteOp::CompleteProcessResponse() {
  end_of_data_ = true;
  return Status::OK();
}

Result<bool> PgDocWriteOp::DoCreateRequests() {
  // Setup a singular operator.
  pgsql_ops_.push_back(write_op_);
  pgsql_ops_.back()->set_active(true);
  ResetResultStream();
  active_op_count_ = 1;

  // Log non buffered request.
  VLOG_IF(1, response_.Valid()) << __PRETTY_FUNCTION__ << ": Sending request for " << this;
  return true;
}

void PgDocWriteOp::SetWriteTime(const HybridTime& write_time) {
  write_op_->SetWriteTime(write_time);
}

LWPgsqlWriteRequestPB& PgDocWriteOp::GetWriteOp(int op_index) {
  return down_cast<PgsqlWriteOp&>(*pgsql_ops_[op_index]).write_request();
}

PgDocOp::SharedPtr MakeDocReadOpWithData(
    const PgSession::ScopedRefPtr& pg_session, PrefetchedDataHolder data) {
  return std::make_shared<PgDocReadOpCached>(pg_session, std::move(data));
}

}  // namespace yb::pggate
