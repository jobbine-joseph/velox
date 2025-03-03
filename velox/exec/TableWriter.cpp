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

#include "velox/exec/TableWriter.h"

#include "HashAggregation.h"
#include "velox/exec/Task.h"

namespace facebook::velox::exec {

TableWriter::TableWriter(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const core::TableWriteNode>& tableWriteNode)
    : Operator(
          driverCtx,
          tableWriteNode->outputType(),
          operatorId,
          tableWriteNode->id(),
          "TableWrite"),
      driverCtx_(driverCtx),
      connectorPool_(driverCtx_->task->addConnectorPoolLocked(
          planNodeId(),
          driverCtx_->pipelineId,
          driverCtx_->driverId,
          operatorType(),
          tableWriteNode->insertTableHandle()->connectorId())),
      insertTableHandle_(
          tableWriteNode->insertTableHandle()->connectorInsertTableHandle()),
      commitStrategy_(tableWriteNode->commitStrategy()) {
  if (tableWriteNode->outputType()->size() == 1) {
    VELOX_USER_CHECK_NULL(tableWriteNode->aggregationNode());
  } else {
    VELOX_USER_CHECK(tableWriteNode->outputType()->equivalent(
        *(TableWriteTraits::outputType(tableWriteNode->aggregationNode()))));
  }

  if (tableWriteNode->aggregationNode() != nullptr) {
    aggregation_ = std::make_unique<HashAggregation>(
        operatorId, driverCtx, tableWriteNode->aggregationNode());
  }
  const auto& connectorId = tableWriteNode->insertTableHandle()->connectorId();
  connector_ = connector::getConnector(connectorId);
  connectorQueryCtx_ = operatorCtx_->createConnectorQueryCtx(
      connectorId, planNodeId(), connectorPool_);

  auto names = tableWriteNode->columnNames();
  auto types = tableWriteNode->columns()->children();

  const auto& inputType = tableWriteNode->sources()[0]->outputType();

  inputMapping_.reserve(types.size());
  for (const auto& name : tableWriteNode->columns()->names()) {
    inputMapping_.emplace_back(inputType->getChildIdx(name));
  }

  mappedType_ = ROW(std::move(names), std::move(types));
}

void TableWriter::initialize() {
  Operator::initialize();
  VELOX_CHECK_NULL(dataSink_);
  createDataSink();
}

void TableWriter::createDataSink() {
  dataSink_ = connector_->createDataSink(
      mappedType_,
      insertTableHandle_,
      connectorQueryCtx_.get(),
      commitStrategy_);
}

void TableWriter::addInput(RowVectorPtr input) {
  if (input->size() == 0) {
    return;
  }

  std::vector<VectorPtr> mappedChildren;
  mappedChildren.reserve(inputMapping_.size());
  for (auto i : inputMapping_) {
    mappedChildren.emplace_back(input->childAt(i));
  }

  auto mappedInput = std::make_shared<RowVector>(
      input->pool(),
      mappedType_,
      input->nulls(),
      input->size(),
      mappedChildren,
      input->getNullCount());

  dataSink_->appendData(mappedInput);
  numWrittenRows_ += input->size();
  updateWrittenBytes();

  if (aggregation_ != nullptr) {
    aggregation_->addInput(input);
  }
}

RowVectorPtr TableWriter::getOutput() {
  // Making sure the output is read only once after the write is fully done.
  if (!noMoreInput_ || finished_) {
    return nullptr;
  }

  if (aggregation_ != nullptr && !aggregation_->isFinished()) {
    const std::string commitContext = createTableCommitContext(false);
    return TableWriteTraits::createAggregationStatsOutput(
        outputType_,
        aggregation_->getOutput(),
        StringView(commitContext),
        pool());
  }

  finished_ = true;
  updateWrittenBytes();

  if (outputType_->size() == 1) {
    // NOTE: this is for non-prestissimo use cases.
    return std::make_shared<RowVector>(
        pool(),
        outputType_,
        nullptr,
        1,
        std::vector<VectorPtr>{std::make_shared<ConstantVector<int64_t>>(
            pool(), 1, false /*isNull*/, BIGINT(), numWrittenRows_)});
  }

  const std::vector<std::string> fragments = dataSink_->finish();

  vector_size_t numOutputRows = fragments.size() + 1;

  // Page layout:
  // row     fragments     context    [partition]    [stats]
  // X         null          X        [null]          [null]
  // null       X            X        [null]          [null]
  // null       X            X        [null]          [null]

  // 1. Set rows column.
  FlatVectorPtr<int64_t> writtenRowsVector =
      BaseVector::create<FlatVector<int64_t>>(BIGINT(), numOutputRows, pool());
  writtenRowsVector->set(0, (int64_t)numWrittenRows_);
  for (int idx = 1; idx < numOutputRows; ++idx) {
    writtenRowsVector->setNull(idx, true);
  }

  // 2. Set fragments column.
  FlatVectorPtr<StringView> fragmentsVector =
      BaseVector::create<FlatVector<StringView>>(
          VARBINARY(), numOutputRows, pool());
  fragmentsVector->setNull(0, true);
  for (int i = 1; i < numOutputRows; ++i) {
    fragmentsVector->set(i, StringView(fragments[i - 1]));
  }

  // 3. Set commitcontext column.
  const std::string commitContext = createTableCommitContext(true);
  auto commitContextVector = std::make_shared<ConstantVector<StringView>>(
      pool(),
      numOutputRows,
      false /*isNull*/,
      VARBINARY(),
      StringView(commitContext));

  std::vector<VectorPtr> columns = {
      writtenRowsVector, fragmentsVector, commitContextVector};

  // 4. Set null statistics columns.
  if (aggregation_ != nullptr) {
    for (int i = TableWriteTraits::kStatsChannel; i < outputType_->size();
         ++i) {
      columns.push_back(BaseVector::createNullConstant(
          outputType_->childAt(i), writtenRowsVector->size(), pool()));
    }
  }

  return std::make_shared<RowVector>(
      pool(), outputType_, nullptr, numOutputRows, columns);
}

std::string TableWriter::createTableCommitContext(bool lastOutput) {
  // clang-format off
    return folly::toJson(
      folly::dynamic::object
          (TableWriteTraits::kLifeSpanContextKey, "TaskWide")
          (TableWriteTraits::kTaskIdContextKey, connectorQueryCtx_->taskId())
          (TableWriteTraits::kCommitStrategyContextKey, commitStrategyToString(commitStrategy_))
          (TableWriteTraits::klastPageContextKey, lastOutput));
  // clang-format on
}

void TableWriter::updateWrittenBytes() {
  const auto writtenBytes = dataSink_->getCompletedBytes();
  auto lockedStats = stats_.wlock();
  lockedStats->physicalWrittenBytes = writtenBytes;
}

// static
RowVectorPtr TableWriteTraits::createAggregationStatsOutput(
    RowTypePtr outputType,
    RowVectorPtr aggregationOutput,
    StringView tableCommitContext,
    velox::memory::MemoryPool* pool) {
  // TODO: record aggregation stats output time.
  if (aggregationOutput == nullptr) {
    return nullptr;
  }
  VELOX_CHECK_GT(aggregationOutput->childrenSize(), 0);
  const vector_size_t numOutputRows = aggregationOutput->childAt(0)->size();
  std::vector<VectorPtr> columns;
  for (int channel = 0; channel < outputType->size(); channel++) {
    if (channel < TableWriteTraits::kContextChannel) {
      // 1. Set null rows column.
      // 2. Set null fragments column.
      columns.push_back(BaseVector::createNullConstant(
          outputType->childAt(channel), numOutputRows, pool));
      continue;
    }
    if (channel == TableWriteTraits::kContextChannel) {
      // 3. Set commitcontext column.
      columns.push_back(std::make_shared<ConstantVector<StringView>>(
          pool,
          numOutputRows,
          false /*isNull*/,
          VARBINARY(),
          std::move(tableCommitContext)));
      continue;
    }
    // 4. Set statistics columns.
    columns.push_back(
        aggregationOutput->childAt(channel - TableWriteTraits::kStatsChannel));
  }
  return std::make_shared<RowVector>(
      pool, outputType, nullptr, numOutputRows, columns);
}

std::string TableWriteTraits::rowCountColumnName() {
  static const std::string kRowCountName = "rows";
  return kRowCountName;
}

std::string TableWriteTraits::fragmentColumnName() {
  static const std::string kFragmentName = "fragments";
  return kFragmentName;
}

std::string TableWriteTraits::contextColumnName() {
  static const std::string kContextName = "commitcontext";
  return kContextName;
}

const TypePtr& TableWriteTraits::rowCountColumnType() {
  static const TypePtr kRowCountType = BIGINT();
  return kRowCountType;
}

const TypePtr& TableWriteTraits::fragmentColumnType() {
  static const TypePtr kFragmentType = VARBINARY();
  return kFragmentType;
}

const TypePtr& TableWriteTraits::contextColumnType() {
  static const TypePtr kContextType = VARBINARY();
  return kContextType;
}

const RowTypePtr TableWriteTraits::outputType(
    const std::shared_ptr<core::AggregationNode>& aggregationNode) {
  static const auto kOutputTypeWithoutStats =
      ROW({rowCountColumnName(), fragmentColumnName(), contextColumnName()},
          {rowCountColumnType(), fragmentColumnType(), contextColumnType()});
  if (aggregationNode == nullptr) {
    return kOutputTypeWithoutStats;
  }
  return kOutputTypeWithoutStats->unionWith(aggregationNode->outputType());
}

folly::dynamic TableWriteTraits::getTableCommitContext(
    const RowVectorPtr& input) {
  VELOX_CHECK_GT(input->size(), 0);
  auto* contextVector =
      input->childAt(kContextChannel)->as<SimpleVector<StringView>>();
  return folly::parseJson(contextVector->valueAt(input->size() - 1));
}

int64_t TableWriteTraits::getRowCount(const RowVectorPtr& output) {
  VELOX_CHECK_GT(output->size(), 0);
  auto rowCountVector =
      output->childAt(kRowCountChannel)->asFlatVector<int64_t>();
  VELOX_CHECK_NOT_NULL(rowCountVector);
  int64_t rowCount{0};
  for (int i = 0; i < output->size(); ++i) {
    if (!rowCountVector->isNullAt(i)) {
      rowCount += rowCountVector->valueAt(i);
    }
  }
  return rowCount;
}
} // namespace facebook::velox::exec
