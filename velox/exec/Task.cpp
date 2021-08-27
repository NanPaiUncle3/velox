/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/exec/Task.h"
#include "velox/codegen/Codegen.h"
#include "velox/common/time/Timer.h"
#include "velox/exec/Exchange.h"
#include "velox/exec/HashBuild.h"
#include "velox/exec/LocalPlanner.h"
#include "velox/exec/Merge.h"
#include "velox/exec/PartitionedOutputBufferManager.h"
#if CODEGEN_ENABLED == 1
#include "velox/experimental/codegen/CodegenLogger.h"
#endif

namespace facebook::velox::exec {

Task::Task(
    const std::string& taskId,
    std::shared_ptr<const core::PlanNode> planNode,
    int destination,
    std::shared_ptr<core::QueryCtx>&& queryCtx,
    ConsumerSupplier consumerSupplier,
    std::function<void(std::exception_ptr)> onError)
    : taskId_(taskId),
      planNode_(planNode),
      destination_(destination),
      queryCtx_(std::move(queryCtx)),
      consumerSupplier_(std::move(consumerSupplier)),
      onError_(onError),
      pool_(queryCtx_->pool()->addScopedChild("task_root")),
      bufferManager_(
          PartitionedOutputBufferManager::getInstance(queryCtx_->host())) {
  if (!pool_->getMemoryUsageTracker()) {
    // Enable eager memory usage tracking for this task's pool and any child
    // pool created from this task.
    pool_->setMemoryUsageTracker(memory::MemoryUsageTracker::create());
  }
}

Task::~Task() {
  try {
    if (hasPartitionedOutput_) {
      if (auto bufferManager = bufferManager_.lock()) {
        bufferManager->removeTask(taskId_);
      }
    }
  } catch (const std::exception& e) {
    LOG(WARNING) << "Caught exception in ~Task(): " << e.what();
  }
}

void Task::start(std::shared_ptr<Task> self, uint32_t maxDrivers) {
  VELOX_CHECK(self->drivers_.empty());
  {
    std::lock_guard<std::mutex> l(self->mutex_);
    self->taskStats_.executionStartTimeMs = getCurrentTimeMs();
  }

#if CODEGEN_ENABLED == 1
  if (self->queryCtx()->codegenEnabled() &&
      self->queryCtx()->codegenConfigurationFilePath().length() != 0) {
    auto codegenLogger =
        std::make_shared<codegen::DefaultLogger>(self->taskId_);
    auto codegen = codegen::Codegen(codegenLogger);
    auto lazyLoading = self->queryCtx()->codegenLazyLoading();
    codegen.initializeFromFile(
        self->queryCtx()->codegenConfigurationFilePath(), lazyLoading);
    auto newPlanNode = codegen.compile(*(self->planNode_));
    self->planNode_ = newPlanNode != nullptr ? newPlanNode : self->planNode_;
  }
#endif

  LocalPlanner::plan(
      self->planNode_, self->consumerSupplier(), &self->driverFactories_);

  for (auto& factory : self->driverFactories_) {
    self->numDrivers_ += std::min(factory->maxDrivers, maxDrivers);
  }
  self->taskStats_.pipelineStats.resize(self->driverFactories_.size());
  self->drivers_.resize(self->numDrivers_);

  auto bufferManager = self->bufferManager_.lock();
  VELOX_CHECK_NOT_NULL(
      bufferManager,
      "Unable to initialize task. "
      "PartitionedOutputBufferManager was already destructed");

  for (auto pipeline = 0; pipeline < self->driverFactories_.size();
       ++pipeline) {
    auto& factory = self->driverFactories_[pipeline];
    auto numDrivers = std::min(factory->maxDrivers, maxDrivers);
    auto partitionedOutputNode = factory->needsPartitionedOutput();
    if (partitionedOutputNode) {
      VELOX_CHECK(
          !self->hasPartitionedOutput_,
          "Only one output pipeline per task is supported");
      self->hasPartitionedOutput_ = true;
      bufferManager->initializeTask(
          self,
          partitionedOutputNode->isBroadcast(),
          partitionedOutputNode->numPartitions(),
          numDrivers);
    }

    std::shared_ptr<ExchangeClient> exchangeClient = nullptr;
    if (factory->needsExchangeClient()) {
      exchangeClient = self->addExchangeClient();
    }

    auto exchangeId = factory->needsLocalExchangeSource();
    if (exchangeId.has_value()) {
      self->createLocalExchangeSources(exchangeId.value(), numDrivers);
    }

    for (int32_t i = 0; i < numDrivers; ++i) {
      self->drivers_.push_back(factory->createDriver(
          std::make_unique<DriverCtx>(self, i, pipeline, numDrivers),
          exchangeClient,
          [self, maxDrivers](size_t i) {
            return i < self->driverFactories_.size()
                ? std::min(self->driverFactories_[i]->maxDrivers, maxDrivers)
                : 0;
          }));
      if (i == 0) {
        self->drivers_.back()->initializeOperatorStats(
            self->taskStats_.pipelineStats[pipeline].operatorStats);
      }
      Driver::enqueue(self->drivers_.back(), self->queryCtx_->executor());
    }
  }
  self->noMoreLocalExchangeProducers();
}

// static
void Task::resume(std::shared_ptr<Task> self) {
  VELOX_CHECK(!self->exception_, "Cannot resume failed task");
  std::lock_guard<std::mutex> l(self->mutex_);
  for (auto& driver : self->drivers_) {
    if (driver) {
      VELOX_CHECK(!driver->isOnThread() && !driver->isTerminated());
      Driver::enqueue(driver, self->queryCtx_->executor());
    }
  }
}

// static
void Task::removeDriver(std::shared_ptr<Task> self, Driver* driver) {
  for (auto& driverPtr : self->drivers_) {
    if (driverPtr.get() == driver) {
      driverPtr = nullptr;
      self->driverClosed(driver);
      return;
    }
  }
  VELOX_CHECK(false, "Trying to delete a Driver twice from its Task");
}

void Task::setMaxSplitSequenceId(
    const core::PlanNodeId& planNodeId,
    long maxSequenceId) {
  std::lock_guard<std::mutex> l(mutex_);
  VELOX_CHECK(state_ == kRunning);

  auto& splitsState = splitsStates_[planNodeId];
  // We could have been sent an old split again, so only change max id, when the
  // new one is greater.
  splitsState.maxSequenceId =
      std::max(splitsState.maxSequenceId, maxSequenceId);
}

bool Task::addSplitWithSequence(
    const core::PlanNodeId& planNodeId,
    exec::Split&& split,
    long sequenceId) {
  std::lock_guard<std::mutex> l(mutex_);
  VELOX_CHECK(state_ == kRunning);

  // The same split can be added again in some systems. The systems that want
  // 'one split processed once only' would use this method and duplicate splits
  // would be ignored.
  auto& splitsState = splitsStates_[planNodeId];
  if (sequenceId > splitsState.maxSequenceId) {
    addSplitLocked(splitsState, std::move(split));
    return true;
  }

  return false;
}

void Task::addSplit(const core::PlanNodeId& planNodeId, exec::Split&& split) {
  std::lock_guard<std::mutex> l(mutex_);
  VELOX_CHECK(state_ == kRunning);

  addSplitLocked(splitsStates_[planNodeId], std::move(split));
}

void Task::addSplitLocked(SplitsState& splitsState, exec::Split&& split) {
  ++taskStats_.numTotalSplits;
  ++taskStats_.numQueuedSplits;

  splitsState.splits.push_back(split);

  if (split.hasGroup()) {
    ++splitsState.groupSplits[split.groupId].numIncompleteSplits;
  }

  if (not splitsState.splitPromises.empty()) {
    splitsState.splitPromises.back().setValue(false);
    splitsState.splitPromises.pop_back();
  }
}

void Task::noMoreSplitsForGroup(
    const core::PlanNodeId& planNodeId,
    int32_t splitGroupId) {
  std::lock_guard<std::mutex> l(mutex_);

  auto& splitsState = splitsStates_[planNodeId];
  splitsState.groupSplits[splitGroupId].noMoreSplits = true;
  checkGroupSplitsCompleteLocked(
      splitsState.groupSplits,
      splitGroupId,
      splitsState.groupSplits.find(splitGroupId));
}

void Task::noMoreSplits(const core::PlanNodeId& planNodeId) {
  std::lock_guard<std::mutex> l(mutex_);

  auto& splitsState = splitsStates_[planNodeId];
  splitsState.noMoreSplits = true;
  for (auto& promise : splitsState.splitPromises) {
    promise.setValue(false);
  }
  splitsState.splitPromises.clear();
}

bool Task::isAllSplitsFinishedLocked() {
  if (taskStats_.numFinishedSplits == taskStats_.numTotalSplits) {
    for (auto& it : splitsStates_) {
      if (not it.second.noMoreSplits) {
        return false;
      }
    }
    return true;
  }
  return false;
}

BlockingReason Task::getSplitOrFuture(
    const core::PlanNodeId& planNodeId,
    exec::Split& split,
    ContinueFuture& future) {
  std::lock_guard<std::mutex> l(mutex_);

  auto& splitsState = splitsStates_[planNodeId];
  if (splitsState.splits.empty()) {
    if (splitsState.noMoreSplits) {
      return BlockingReason::kNotBlocked;
    }
    auto [splitPromise, splitFuture] = makeVeloxPromiseContract<bool>(
        fmt::format("Task::getSplitOrFuture {}", taskId_));
    future = std::move(splitFuture);
    splitsState.splitPromises.push_back(std::move(splitPromise));
    return BlockingReason::kWaitForSplit;
  }

  split = std::move(splitsState.splits.front());
  splitsState.splits.pop_front();

  --taskStats_.numQueuedSplits;
  ++taskStats_.numRunningSplits;

  if (taskStats_.firstSplitStartTimeMs == 0) {
    taskStats_.firstSplitStartTimeMs = getCurrentTimeMs();
  }
  taskStats_.lastSplitStartTimeMs = getCurrentTimeMs();

  return BlockingReason::kNotBlocked;
}

void Task::splitFinished(
    const core::PlanNodeId& planNodeId,
    int32_t splitGroupId) {
  std::lock_guard<std::mutex> l(mutex_);
  ++taskStats_.numFinishedSplits;
  --taskStats_.numRunningSplits;
  if (isAllSplitsFinishedLocked()) {
    taskStats_.executionEndTimeMs = getCurrentTimeMs();
  }
  // If bucketed group id for this split is valid, we want to check if this
  // group has been completed (no more running or queued splits).
  if (splitGroupId != -1) {
    auto& splitsState = splitsStates_[planNodeId];
    auto it = splitsState.groupSplits.find(splitGroupId);
    VELOX_DCHECK(
        it != splitsState.groupSplits.end(),
        "We have a finished split in group {}, which wasn't registered!",
        splitGroupId);
    if (it != splitsState.groupSplits.end()) {
      --it->second.numIncompleteSplits;
      VELOX_DCHECK_GE(
          it->second.numIncompleteSplits,
          0,
          "Number of incomplete splits in group {} is negative: {}!",
          splitGroupId,
          it->second.numIncompleteSplits);
      checkGroupSplitsCompleteLocked(splitsState.groupSplits, splitGroupId, it);
    }
  }
}

void Task::multipleSplitsFinished(int32_t numSplits) {
  std::lock_guard<std::mutex> l(mutex_);
  taskStats_.numFinishedSplits += numSplits;
  taskStats_.numRunningSplits -= numSplits;
}

void Task::checkGroupSplitsCompleteLocked(
    std::unordered_map<int32_t, GroupSplitsInfo>& mapGroupSplits,
    int32_t splitGroupId,
    std::unordered_map<int32_t, GroupSplitsInfo>::iterator it) {
  if (it->second.numIncompleteSplits == 0 and it->second.noMoreSplits) {
    mapGroupSplits.erase(it);
    taskStats_.completedSplitGroups.emplace(splitGroupId);
  }
}

void Task::updateBroadcastOutputBuffers(int numBuffers, bool noMoreBuffers) {
  auto bufferManager = bufferManager_.lock();
  VELOX_CHECK_NOT_NULL(
      bufferManager,
      "Unable to initialize task. "
      "PartitionedOutputBufferManager was already destructed");

  bufferManager->updateBroadcastOutputBuffers(
      taskId_, numBuffers, noMoreBuffers);
}

void Task::setAllOutputConsumed() {
  std::lock_guard<std::mutex> l(mutex_);
  partitionedOutputConsumed_ = true;
  if (!numDrivers_ && state_ == kRunning) {
    state_ = kFinished;
    stateChangedLocked();
  }
}

void Task::driverClosed(Driver* /* unused */) {
  std::lock_guard<std::mutex> cancelPoolLock(*cancelPool()->mutex());
  --numDrivers_;
  if ((numDrivers_ == 0) && (state_ == kRunning)) {
    std::lock_guard<std::mutex> l(mutex_);
    if (taskStats_.executionEndTimeMs == 0) {
      // In case we haven't set executionEndTimeMs due to all splits depleted,
      // we set it here.
      // This can happen due to task error or task being cancelled.
      taskStats_.executionEndTimeMs = getCurrentTimeMs();
    }
    if (!hasPartitionedOutput_ || partitionedOutputConsumed_) {
      state_ = kFinished;
      stateChangedLocked();
    }
  }
}

std::shared_ptr<ExchangeClient> Task::addExchangeClient() {
  exchangeClients_.emplace_back(std::make_shared<ExchangeClient>(destination_));
  return exchangeClients_.back();
}

bool Task::allPeersFinished(
    const core::PlanNodeId& planNodeId,
    Driver* caller,
    ContinueFuture* future,
    std::vector<VeloxPromise<bool>>& promises,
    std::vector<std::shared_ptr<Driver>>& peers) {
  std::lock_guard<std::mutex> l(mutex_);
  if (exception_) {
    VELOX_FAIL("Task is terminating because of error: {}", errorMessage());
  }
  auto& state = barriers_[planNodeId];

  if (++state.numRequested == caller->driverCtx()->numDrivers) {
    peers = std::move(state.drivers);
    promises = std::move(state.promises);
    barriers_.erase(planNodeId);
    return true;
  }
  std::shared_ptr<Driver> callerShared;
  for (auto& driver : drivers_) {
    if (driver.get() == caller) {
      callerShared = driver;
      break;
    }
  }
  VELOX_CHECK(callerShared, "Caller of pipelineBarrier is not a valid Driver");
  state.drivers.push_back(callerShared);
  state.promises.emplace_back(
      fmt::format("Task::allPeersFinished {}", taskId_));
  *future = state.promises.back().getSemiFuture();

  return false;
}

std::shared_ptr<JoinBridge> Task::findOrCreateJoinBridge(
    const core::PlanNodeId& planNodeId) {
  std::lock_guard<std::mutex> l(mutex_);
  auto& bridge = bridges_[planNodeId];
  if (!bridge) {
    bridge = std::make_shared<JoinBridge>();
    bridges_[planNodeId] = bridge;
  }
  return bridge;
}

//  static
std::string Task::shortId(const std::string& id) {
  if (id.size() < 12) {
    return id;
  }
  const char* str = id.c_str();
  const char* dot = strchr(str, '.');
  if (!dot) {
    return id;
  }
  auto hash = std::hash<std::string_view>()(std::string_view(str, dot - str));
  return fmt::format("tk:{}", hash & 0xffff);
}

void Task::terminate(TaskState terminalState) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (taskStats_.executionEndTimeMs == 0) {
      taskStats_.executionEndTimeMs = getCurrentTimeMs();
    }
    if (state_ != kRunning) {
      return;
    }
    state_ = terminalState;
  }
  cancelPool()->requestTerminate();
  for (auto driver : drivers_) {
    // 'driver' is a  copy of the shared_ptr in
    // 'drivers_'. This is safe against a concurrent remove of the
    // Driver.
    if (driver) {
      driver->terminate();
    }
  }
  // We continue all Drivers waiting for splits or space in exchange buffers.
  if (hasPartitionedOutput_) {
    if (auto bufferManager = bufferManager_.lock()) {
      bufferManager->removeTask(taskId_);
    }
  }
  // Release reference to exchange client, so that it will close exchange
  // sources and prevent resending requests for data.
  std::lock_guard<std::mutex> l(mutex_);
  exchangeClients_.clear();
  for (auto& pair : splitsStates_) {
    for (auto& promise : pair.second.splitPromises) {
      promise.setValue(true);
    }
    pair.second.splitPromises.clear();
  }

  for (auto& pair : bridges_) {
    pair.second->cancel();
  }
  stateChangedLocked();
}

void Task::addOperatorStats(OperatorStats& stats) {
  std::lock_guard<std::mutex> l(mutex_);
  VELOX_CHECK(
      stats.pipelineId >= 0 &&
      stats.pipelineId < taskStats_.pipelineStats.size());
  VELOX_CHECK(
      stats.operatorId >= 0 &&
      stats.operatorId <
          taskStats_.pipelineStats[stats.pipelineId].operatorStats.size());
  taskStats_.pipelineStats[stats.pipelineId]
      .operatorStats[stats.operatorId]
      .add(stats);
  stats.clear();
}

uint64_t Task::timeSinceStartMs() const {
  std::lock_guard<std::mutex> l(mutex_);
  if (taskStats_.executionStartTimeMs == 0UL) {
    return 0UL;
  }
  return getCurrentTimeMs() - taskStats_.executionStartTimeMs;
}

uint64_t Task::timeSinceEndMs() const {
  std::lock_guard<std::mutex> l(mutex_);
  if (taskStats_.executionEndTimeMs == 0UL) {
    return 0UL;
  }
  return getCurrentTimeMs() - taskStats_.executionEndTimeMs;
}

void Task::stateChangedLocked() {
  for (auto& promise : stateChangePromises_) {
    promise.setValue(true);
  }
  stateChangePromises_.clear();
}

ContinueFuture Task::stateChangeFuture(uint64_t maxWaitMicros) {
  std::lock_guard<std::mutex> l(mutex_);
  // If 'this' is running, the future is realized on timeout or when
  // this no longer is running.
  if (state_ != kRunning) {
    return ContinueFuture(true);
  }
  auto [promise, future] = makeVeloxPromiseContract<bool>(
      fmt::format("Task::stateChangeFuture {}", taskId_));
  stateChangePromises_.emplace_back(std::move(promise));
  if (maxWaitMicros) {
    return std::move(future).within(std::chrono::microseconds(maxWaitMicros));
  }
  return std::move(future);
}

std::string Task::toString() {
  std::stringstream out;
  out << "{Task " << shortId(taskId_) << " (" << taskId_ << ")";

  if (exception_) {
    out << "Error: " << errorMessage() << std::endl;
  }

  if (planNode_) {
    out << "Plan: " << planNode_->toString() << std::endl;
  }

  out << " drivers:\n";
  for (auto& driver : drivers_) {
    if (driver) {
      out << driver->toString() << std::endl;
    }
  }

  for (const auto& pair : splitsStates_) {
    const auto& splitState = pair.second;
    out << "Plan Node: " << pair.first << ": " << std::endl;

    out << splitState.splits.size() << " splits: ";
    int32_t counter = 0;
    for (const auto& split : splitState.splits) {
      out << split.toString() << " ";
      if (++counter > 4) {
        out << "...";
        break;
      }
    }
    out << std::endl;

    if (splitState.noMoreSplits) {
      out << "No more splits" << std::endl;
    }

    if (not splitState.splitPromises.empty()) {
      out << splitState.splitPromises.size() << " split promises" << std::endl;
    }
  }

  return out.str();
}

void Task::createLocalMergeSources(
    unsigned numSources,
    const std::shared_ptr<const RowType>& rowType,
    memory::MappedMemory* mappedMemory) {
  VELOX_CHECK(
      localMergeSources_.empty(),
      "Multiple local merges in a single task not supported");
  localMergeSources_.reserve(numSources);
  for (auto i = 0; i < numSources; ++i) {
    localMergeSources_.emplace_back(
        MergeSource::createLocalMergeSource(rowType, mappedMemory));
  }
}

void Task::createLocalExchangeSources(
    const core::PlanNodeId& planNodeId,
    int numPartitions) {
  VELOX_CHECK(
      localExchanges_.find(planNodeId) == localExchanges_.end(),
      "Local exchange already exists: {}",
      planNodeId);

  LocalExchange exchange;
  exchange.memoryManager = std::make_unique<LocalExchangeMemoryManager>(
      queryCtx_->maxLocalExchangeBufferSize());

  exchange.sources.reserve(numPartitions);
  for (auto i = 0; i < numPartitions; ++i) {
    exchange.sources.emplace_back(
        std::make_shared<LocalExchangeSource>(exchange.memoryManager.get(), i));
  }

  localExchanges_.insert({planNodeId, std::move(exchange)});
}

void Task::noMoreLocalExchangeProducers() {
  for (auto& exchange : localExchanges_) {
    for (auto& source : exchange.second.sources) {
      source->noMoreProducers();
    }
  }
}

std::shared_ptr<LocalExchangeSource> Task::getLocalExchangeSource(
    const core::PlanNodeId& planNodeId,
    int partition) {
  const auto& sources = getLocalExchangeSources(planNodeId);
  VELOX_CHECK_LT(
      partition,
      sources.size(),
      "Incorrect partition for local exchange {}",
      planNodeId);
  return sources[partition];
}

const std::vector<std::shared_ptr<LocalExchangeSource>>&
Task::getLocalExchangeSources(const core::PlanNodeId& planNodeId) {
  auto it = localExchanges_.find(planNodeId);
  VELOX_CHECK(
      it != localExchanges_.end(),
      "Incorrect local exchange ID: {}",
      planNodeId);
  return it->second.sources;
}

} // namespace facebook::velox::exec
