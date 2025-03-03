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

#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/connectors/hive/HiveConnector.h"
#include "velox/connectors/tpch/TpchConnector.h"
#include "velox/duckdb/conversion/DuckParser.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/HashPartitionFunction.h"
#include "velox/exec/RoundRobinPartitionFunction.h"
#include "velox/exec/TableWriter.h"
#include "velox/exec/WindowFunction.h"
#include "velox/expression/ExprToSubfieldFilter.h"
#include "velox/expression/SignatureBinder.h"
#include "velox/parse/Expressions.h"

#ifndef VELOX_ENABLE_BACKWARD_COMPATIBILITY
#include "velox/connectors/hive/TableHandle.h"
#include "velox/expression/Expr.h"
#else
#include <velox/core/ITypedExpr.h>
#include "velox/common/memory/Memory.h"
#include "velox/parse/ExpressionsParser.h"
#endif

using namespace facebook::velox;
using namespace facebook::velox::connector;
using namespace facebook::velox::connector::hive;

namespace facebook::velox::exec::test {

namespace {

// TODO Avoid duplication.
static const std::string kHiveConnectorId = "test-hive";
static const std::string kTpchConnectorId = "test-tpch";

core::TypedExprPtr parseExpr(
    const std::string& text,
    const RowTypePtr& rowType,
    const parse::ParseOptions& options,
    memory::MemoryPool* pool) {
  auto untyped = parse::parseExpr(text, options);
  return core::Expressions::inferTypes(untyped, rowType, pool);
}

template <TypeKind FromKind, TypeKind ToKind>
typename TypeTraits<ToKind>::NativeType cast(const variant& v) {
  return util::Converter<ToKind, void, false>::cast(v.value<FromKind>());
}
} // namespace

PlanBuilder& PlanBuilder::tableScan(
    const RowTypePtr& outputType,
    const std::vector<std::string>& subfieldFilters,
    const std::string& remainingFilter) {
  return tableScan(
      "hive_table", outputType, {}, subfieldFilters, remainingFilter);
}

PlanBuilder& PlanBuilder::tableScan(
    const std::string& tableName,
    const RowTypePtr& outputType,
    const std::unordered_map<std::string, std::string>& columnAliases,
    const std::vector<std::string>& subfieldFilters,
    const std::string& remainingFilter) {
  std::unordered_map<std::string, std::shared_ptr<connector::ColumnHandle>>
      assignments;
  std::unordered_map<std::string, core::TypedExprPtr> typedMapping;
  for (uint32_t i = 0; i < outputType->size(); ++i) {
    const auto& name = outputType->nameOf(i);
    const auto& type = outputType->childAt(i);

    std::string hiveColumnName = name;
    auto it = columnAliases.find(name);
    if (it != columnAliases.end()) {
      hiveColumnName = it->second;
      typedMapping.emplace(
          name,
          std::make_shared<core::FieldAccessTypedExpr>(type, hiveColumnName));
    }

    assignments.insert(
        {name,
         std::make_shared<HiveColumnHandle>(
             hiveColumnName,
             HiveColumnHandle::ColumnType::kRegular,
             type,
             type)});
  }
  SubfieldFilters filters;
  filters.reserve(subfieldFilters.size());
  core::QueryCtx queryCtx;
  exec::SimpleExpressionEvaluator evaluator(&queryCtx, pool_);
  for (const auto& filter : subfieldFilters) {
    auto filterExpr = parseExpr(filter, outputType, options_, pool_);
    auto [subfield, subfieldFilter] =
        exec::toSubfieldFilter(filterExpr, &evaluator);

    auto it = columnAliases.find(subfield.toString());
    if (it != columnAliases.end()) {
      subfield = common::Subfield(it->second);
    }

    VELOX_CHECK_EQ(
        filters.count(subfield),
        0,
        "Duplicate subfield: {}",
        subfield.toString());

    filters[std::move(subfield)] = std::move(subfieldFilter);
  }

  core::TypedExprPtr remainingFilterExpr;
  if (!remainingFilter.empty()) {
    remainingFilterExpr =
        parseExpr(remainingFilter, outputType, options_, pool_)
            ->rewriteInputNames(typedMapping);
  }

  auto tableHandle = std::make_shared<HiveTableHandle>(
      kHiveConnectorId,
      tableName,
      true,
      std::move(filters),
      remainingFilterExpr,
      nullptr);
  return tableScan(outputType, tableHandle, assignments);
}

PlanBuilder& PlanBuilder::tableScan(
    const RowTypePtr& outputType,
    const std::shared_ptr<connector::ConnectorTableHandle>& tableHandle,
    const std::unordered_map<
        std::string,
        std::shared_ptr<connector::ColumnHandle>>& assignments) {
  planNode_ = std::make_shared<core::TableScanNode>(
      nextPlanNodeId(), outputType, tableHandle, assignments);
  return *this;
}

PlanBuilder& PlanBuilder::tableScan(
    tpch::Table table,
    std::vector<std::string>&& columnNames,
    double scaleFactor) {
  std::unordered_map<std::string, std::shared_ptr<connector::ColumnHandle>>
      assignmentsMap;
  std::vector<TypePtr> outputTypes;

  assignmentsMap.reserve(columnNames.size());
  outputTypes.reserve(columnNames.size());

  for (const auto& columnName : columnNames) {
    assignmentsMap.emplace(
        columnName,
        std::make_shared<connector::tpch::TpchColumnHandle>(columnName));
    outputTypes.emplace_back(resolveTpchColumn(table, columnName));
  }
  auto rowType = ROW(std::move(columnNames), std::move(outputTypes));
  return tableScan(
      rowType,
      std::make_shared<connector::tpch::TpchTableHandle>(
          kTpchConnectorId, table, scaleFactor),
      assignmentsMap);
}

PlanBuilder& PlanBuilder::values(
    const std::vector<RowVectorPtr>& values,
    bool parallelizable,
    size_t repeatTimes) {
  auto valuesCopy = values;
  planNode_ = std::make_shared<core::ValuesNode>(
      nextPlanNodeId(), std::move(valuesCopy), parallelizable, repeatTimes);
  return *this;
}

PlanBuilder& PlanBuilder::exchange(const RowTypePtr& outputType) {
  VELOX_CHECK_NULL(planNode_, "exchange() must be the first call");
  planNode_ =
      std::make_shared<core::ExchangeNode>(nextPlanNodeId(), outputType);
  return *this;
}

namespace {
std::pair<
    std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>,
    std::vector<core::SortOrder>>
parseOrderByClauses(
    const std::vector<std::string>& keys,
    const RowTypePtr& inputType,
    memory::MemoryPool* pool) {
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>> sortingKeys;
  std::vector<core::SortOrder> sortingOrders;
  for (const auto& key : keys) {
    auto [untypedExpr, sortOrder] = parse::parseOrderByExpr(key);
    auto typedExpr =
        core::Expressions::inferTypes(untypedExpr, inputType, pool);

    auto sortingKey =
        std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(typedExpr);
    VELOX_CHECK_NOT_NULL(
        sortingKey,
        "ORDER BY clause must use a column name, not an expression: {}",
        key);
    sortingKeys.emplace_back(sortingKey);
    sortingOrders.emplace_back(sortOrder);
  }

  return {sortingKeys, sortingOrders};
}
} // namespace

PlanBuilder& PlanBuilder::mergeExchange(
    const RowTypePtr& outputType,
    const std::vector<std::string>& keys) {
  auto [sortingKeys, sortingOrders] =
      parseOrderByClauses(keys, outputType, pool_);

  planNode_ = std::make_shared<core::MergeExchangeNode>(
      nextPlanNodeId(), outputType, sortingKeys, sortingOrders);

  return *this;
}

PlanBuilder& PlanBuilder::optionalProject(
    const std::vector<std::string>& optionalProjections) {
  if (optionalProjections.empty()) {
    return *this;
  }
  return project(optionalProjections);
}

PlanBuilder& PlanBuilder::project(const std::vector<std::string>& projections) {
  std::vector<core::TypedExprPtr> expressions;
  std::vector<std::string> projectNames;
  for (auto i = 0; i < projections.size(); ++i) {
    auto untypedExpr = parse::parseExpr(projections[i], options_);
    expressions.push_back(inferTypes(untypedExpr));
    if (untypedExpr->alias().has_value()) {
      projectNames.push_back(untypedExpr->alias().value());
    } else if (
        auto fieldExpr =
            dynamic_cast<const core::FieldAccessExpr*>(untypedExpr.get())) {
      projectNames.push_back(fieldExpr->getFieldName());
    } else {
      projectNames.push_back(fmt::format("p{}", i));
    }
  }
  planNode_ = std::make_shared<core::ProjectNode>(
      nextPlanNodeId(),
      std::move(projectNames),
      std::move(expressions),
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::optionalFilter(const std::string& optionalFilter) {
  if (optionalFilter.empty()) {
    return *this;
  }
  return filter(optionalFilter);
}

PlanBuilder& PlanBuilder::filter(const std::string& filter) {
  planNode_ = std::make_shared<core::FilterNode>(
      nextPlanNodeId(),
      parseExpr(filter, planNode_->outputType(), options_, pool_),
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::tableWrite(
    const std::vector<std::string>& tableColumnNames,
    const std::shared_ptr<core::AggregationNode>& aggregationNode,
    const std::shared_ptr<core::InsertTableHandle>& insertHandle,
    bool hasPartitioningScheme,
    CommitStrategy commitStrategy) {
  return tableWrite(
      planNode_->outputType(),
      tableColumnNames,
      aggregationNode,
      insertHandle,
      hasPartitioningScheme,
      commitStrategy);
}

PlanBuilder& PlanBuilder::tableWrite(
    const RowTypePtr& inputColumns,
    const std::vector<std::string>& tableColumnNames,
    const std::shared_ptr<core::AggregationNode>& aggregationNode,
    const std::shared_ptr<core::InsertTableHandle>& insertHandle,
    bool hasPartitioningScheme,
    CommitStrategy commitStrategy) {
  planNode_ = std::make_shared<core::TableWriteNode>(
      nextPlanNodeId(),
      inputColumns,
      tableColumnNames,
      aggregationNode,
      insertHandle,
      hasPartitioningScheme,
      TableWriteTraits::outputType(aggregationNode),
      commitStrategy,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::tableWriteMerge(
    const std::shared_ptr<core::AggregationNode>& aggregationNode) {
  planNode_ = std::make_shared<core::TableWriteMergeNode>(
      nextPlanNodeId(),
      TableWriteTraits::outputType(aggregationNode),
      aggregationNode,
      planNode_);
  return *this;
}

namespace {

std::string throwAggregateFunctionDoesntExist(const std::string& name) {
  std::stringstream error;
  error << "Aggregate function doesn't exist: " << name << ".";
  exec::aggregateFunctions().withRLock([&](const auto& functionsMap) {
    if (functionsMap.empty()) {
      error << " Registry of aggregate functions is empty. "
               "Make sure to register some aggregate functions.";
    }
  });
  VELOX_USER_FAIL(error.str());
}

std::string throwAggregateFunctionSignatureNotSupported(
    const std::string& name,
    const std::vector<TypePtr>& types,
    const std::vector<std::shared_ptr<AggregateFunctionSignature>>&
        signatures) {
  std::stringstream error;
  error << "Aggregate function signature is not supported: "
        << toString(name, types)
        << ". Supported signatures: " << toString(signatures) << ".";
  VELOX_USER_FAIL(error.str());
}

TypePtr resolveAggregateType(
    const std::string& aggregateName,
    core::AggregationNode::Step step,
    const std::vector<TypePtr>& rawInputTypes,
    bool nullOnFailure) {
  if (auto signatures = exec::getAggregateFunctionSignatures(aggregateName)) {
    for (const auto& signature : signatures.value()) {
      exec::SignatureBinder binder(*signature, rawInputTypes);
      if (binder.tryBind()) {
        return binder.tryResolveType(
            exec::isPartialOutput(step) ? signature->intermediateType()
                                        : signature->returnType());
      }
    }

    if (nullOnFailure) {
      return nullptr;
    }

    throwAggregateFunctionSignatureNotSupported(
        aggregateName, rawInputTypes, signatures.value());
  }

  if (nullOnFailure) {
    return nullptr;
  }

  throwAggregateFunctionDoesntExist(aggregateName);
  return nullptr;
}

class AggregateTypeResolver {
 public:
  explicit AggregateTypeResolver(core::AggregationNode::Step step)
      : step_(step), previousHook_(core::Expressions::getResolverHook()) {
    core::Expressions::setTypeResolverHook(
        [&](const auto& inputs, const auto& expr, bool nullOnFailure) {
          return resolveType(inputs, expr, nullOnFailure);
        });
  }

  ~AggregateTypeResolver() {
    core::Expressions::setTypeResolverHook(previousHook_);
  }

  void setResultType(const TypePtr& type) {
    resultType_ = type;
  }

 private:
  TypePtr resolveType(
      const std::vector<core::TypedExprPtr>& inputs,
      const std::shared_ptr<const core::CallExpr>& expr,
      bool nullOnFailure) const {
    if (resultType_) {
      return resultType_;
    }

    std::vector<TypePtr> types;
    for (auto& input : inputs) {
      types.push_back(input->type());
    }

    auto functionName = expr->getFunctionName();

    // Use raw input types (if available) to resolve intermediate and final
    // result types.
    if (exec::isRawInput(step_)) {
      return resolveAggregateType(functionName, step_, types, nullOnFailure);
    }

    if (!nullOnFailure) {
      VELOX_USER_FAIL(
          "Cannot resolve aggregation function return type without raw input types: {}",
          functionName);
    }
    return nullptr;
  }

  const core::AggregationNode::Step step_;
  const core::Expressions::TypeResolverHook previousHook_;
  TypePtr resultType_;
};

} // namespace

core::PlanNodePtr PlanBuilder::createIntermediateOrFinalAggregation(
    core::AggregationNode::Step step,
    const core::AggregationNode* partialAggNode) {
  // Create intermediate or final aggregation using same grouping keys and same
  // aggregate function names.
  const auto& partialAggregates = partialAggNode->aggregates();
  const auto& groupingKeys = partialAggNode->groupingKeys();

  auto numAggregates = partialAggregates.size();
  auto numGroupingKeys = groupingKeys.size();

  std::vector<core::AggregationNode::Aggregate> aggregates;
  aggregates.reserve(numAggregates);
  for (auto i = 0; i < numAggregates; i++) {
    // Resolve final or intermediate aggregation result type using raw input
    // types for the partial aggregation.
    auto name = partialAggregates[i].call->name();
    auto rawInputs = partialAggregates[i].call->inputs();

    std::vector<TypePtr> rawInputTypes;
    for (auto& rawInput : rawInputs) {
      rawInputTypes.push_back(rawInput->type());
    }

    core::AggregationNode::Aggregate aggregate;

    auto type = resolveAggregateType(name, step, rawInputTypes, false);
    std::vector<core::TypedExprPtr> inputs = {field(numGroupingKeys + i)};
    aggregate.call =
        std::make_shared<core::CallTypedExpr>(type, std::move(inputs), name);
    aggregates.emplace_back(aggregate);
  }

  return std::make_shared<core::AggregationNode>(
      nextPlanNodeId(),
      step,
      groupingKeys,
      partialAggNode->preGroupedKeys(),
      partialAggNode->aggregateNames(),
      aggregates,
      partialAggNode->ignoreNullKeys(),
      planNode_);
}

namespace {
/// Checks that specified plan node is a partial or intermediate aggregation or
/// local exchange over the same. Returns a pointer to core::AggregationNode.
const core::AggregationNode* findPartialAggregation(
    const core::PlanNode* planNode) {
  const core::AggregationNode* aggNode;
  if (auto exchange = dynamic_cast<const core::LocalPartitionNode*>(planNode)) {
    aggNode = dynamic_cast<const core::AggregationNode*>(
        exchange->sources()[0].get());
  } else if (auto merge = dynamic_cast<const core::LocalMergeNode*>(planNode)) {
    aggNode =
        dynamic_cast<const core::AggregationNode*>(merge->sources()[0].get());
  } else {
    aggNode = dynamic_cast<const core::AggregationNode*>(planNode);
  }
  VELOX_CHECK_NOT_NULL(
      aggNode,
      "Current plan node must be one of: partial or intermediate aggregation, "
      "local merge or exchange. Got: {}",
      planNode->toString());
  VELOX_CHECK(exec::isPartialOutput(aggNode->step()));
  return aggNode;
}
} // namespace

PlanBuilder& PlanBuilder::intermediateAggregation() {
  const auto* aggNode = findPartialAggregation(planNode_.get());
  VELOX_CHECK(exec::isRawInput(aggNode->step()));

  auto step = core::AggregationNode::Step::kIntermediate;

  planNode_ = createIntermediateOrFinalAggregation(step, aggNode);
  return *this;
}

PlanBuilder& PlanBuilder::finalAggregation() {
  const auto* aggNode = findPartialAggregation(planNode_.get());

  if (!exec::isRawInput(aggNode->step())) {
    // If aggregation node is not the partial aggregation, keep looking again.
    aggNode = findPartialAggregation(aggNode->sources()[0].get());
    if (!exec::isRawInput(aggNode->step())) {
      VELOX_CHECK_NOT_NULL(
          aggNode,
          "Plan node before current plan node must be a partial aggregation.");
      VELOX_CHECK(exec::isRawInput(aggNode->step()));
      VELOX_CHECK(exec::isPartialOutput(aggNode->step()));
    }
  }

  auto step = core::AggregationNode::Step::kFinal;

  planNode_ = createIntermediateOrFinalAggregation(step, aggNode);
  return *this;
}

PlanBuilder::AggregatesAndNames PlanBuilder::createAggregateExpressionsAndNames(
    const std::vector<std::string>& aggregates,
    const std::vector<std::string>& masks,
    core::AggregationNode::Step step,
    const std::vector<TypePtr>& resultTypes) {
  std::vector<core::AggregationNode::Aggregate> aggs;

  AggregateTypeResolver resolver(step);
  std::vector<std::string> names;
  aggs.reserve(aggregates.size());
  names.reserve(aggregates.size());

  duckdb::ParseOptions options;
  options.parseIntegerAsBigint = options_.parseIntegerAsBigint;

  for (auto i = 0; i < aggregates.size(); i++) {
    auto& aggregate = aggregates[i];
    if (i < resultTypes.size()) {
      resolver.setResultType(resultTypes[i]);
    }

    auto untypedExpr = duckdb::parseAggregateExpr(aggregate, options);

    core::AggregationNode::Aggregate agg;
    agg.call = std::dynamic_pointer_cast<const core::CallTypedExpr>(
        inferTypes(untypedExpr.expr));
    if (untypedExpr.maskExpr != nullptr) {
      auto maskExpr =
          std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(
              inferTypes(untypedExpr.maskExpr));
      VELOX_CHECK_NOT_NULL(
          maskExpr,
          "FILTER clause must use a column name, not an expression: {}",
          aggregate);
      agg.mask = maskExpr;
    }

    if (i < masks.size() && !masks[i].empty()) {
      VELOX_CHECK_NULL(
          agg.mask,
          "Aggregation mask should be specified only once (either explicitly or using FILTER clause)");
      agg.mask = field(masks[i]);
    }

    agg.distinct = untypedExpr.distinct;

    if (!untypedExpr.orderBy.empty()) {
      VELOX_CHECK(
          step == core::AggregationNode::Step::kSingle,
          "Aggregations over sorted inputs cannot be split into partial and final: {}.",
          aggregate)
    }

    for (const auto& [keyExpr, order] : untypedExpr.orderBy) {
      auto sortingKey =
          std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(
              inferTypes(keyExpr));
      VELOX_CHECK_NOT_NULL(
          sortingKey,
          "ORDER BY clause must use a column name, not an expression: {}",
          aggregate);

      agg.sortingKeys.push_back(sortingKey);
      agg.sortingOrders.push_back(order);
    }

    aggs.emplace_back(agg);

    if (untypedExpr.expr->alias().has_value()) {
      names.push_back(untypedExpr.expr->alias().value());
    } else {
      names.push_back(fmt::format("a{}", i));
    }
  }

  return {aggs, names};
}

PlanBuilder& PlanBuilder::aggregation(
    const std::vector<std::string>& groupingKeys,
    const std::vector<std::string>& preGroupedKeys,
    const std::vector<std::string>& aggregates,
    const std::vector<std::string>& masks,
    core::AggregationNode::Step step,
    bool ignoreNullKeys,
    const std::vector<TypePtr>& resultTypes) {
  auto aggregatesAndNames =
      createAggregateExpressionsAndNames(aggregates, masks, step, resultTypes);
  planNode_ = std::make_shared<core::AggregationNode>(
      nextPlanNodeId(),
      step,
      fields(groupingKeys),
      fields(preGroupedKeys),
      aggregatesAndNames.names,
      aggregatesAndNames.aggregates,
      ignoreNullKeys,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::streamingAggregation(
    const std::vector<std::string>& groupingKeys,
    const std::vector<std::string>& aggregates,
    const std::vector<std::string>& masks,
    core::AggregationNode::Step step,
    bool ignoreNullKeys,
    const std::vector<TypePtr>& resultTypes) {
  auto aggregatesAndNames =
      createAggregateExpressionsAndNames(aggregates, masks, step, resultTypes);
  planNode_ = std::make_shared<core::AggregationNode>(
      nextPlanNodeId(),
      step,
      fields(groupingKeys),
      fields(groupingKeys),
      aggregatesAndNames.names,
      aggregatesAndNames.aggregates,
      ignoreNullKeys,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::groupId(
    const std::vector<std::vector<std::string>>& groupingSets,
    const std::vector<std::string>& aggregationInputs,
    std::string groupIdName) {
  std::vector<std::vector<core::FieldAccessTypedExprPtr>> groupingSetExprs;
  groupingSetExprs.reserve(groupingSets.size());
  for (const auto& groupingSet : groupingSets) {
    groupingSetExprs.push_back(fields(groupingSet));
  }

  std::vector<core::GroupIdNode::GroupingKeyInfo> groupingKeyInfos;
  std::set<std::string> names;
  auto index = 0;
  for (const auto& groupingSet : groupingSetExprs) {
    for (const auto& groupingKey : groupingSet) {
      if (names.find(groupingKey->name()) == names.end()) {
        core::GroupIdNode::GroupingKeyInfo keyInfos;
        keyInfos.output = groupingKey->name();
        keyInfos.input = groupingKey;
        groupingKeyInfos.push_back(keyInfos);
      }
      names.insert(groupingKey->name());
    }
  }

  planNode_ = std::make_shared<core::GroupIdNode>(
      nextPlanNodeId(),
      groupingSetExprs,
      std::move(groupingKeyInfos),
      fields(aggregationInputs),
      std::move(groupIdName),
      planNode_);

  return *this;
}

namespace {
core::PlanNodePtr createLocalMergeNode(
    const core::PlanNodeId& id,
    const std::vector<std::string>& keys,
    std::vector<core::PlanNodePtr> sources,
    memory::MemoryPool* pool) {
  const auto& inputType = sources[0]->outputType();
  auto [sortingKeys, sortingOrders] =
      parseOrderByClauses(keys, inputType, pool);

  return std::make_shared<core::LocalMergeNode>(
      id, std::move(sortingKeys), std::move(sortingOrders), std::move(sources));
}
} // namespace

PlanBuilder& PlanBuilder::localMerge(const std::vector<std::string>& keys) {
  planNode_ = createLocalMergeNode(nextPlanNodeId(), keys, {planNode_}, pool_);
  return *this;
}

PlanBuilder& PlanBuilder::localMerge(
    const std::vector<std::string>& keys,
    std::vector<core::PlanNodePtr> sources) {
  VELOX_CHECK_NULL(planNode_, "localMerge() must be the first call");
  VELOX_CHECK_GE(
      sources.size(), 1, "localMerge() requires at least one source");

  planNode_ =
      createLocalMergeNode(nextPlanNodeId(), keys, std::move(sources), pool_);
  return *this;
}

PlanBuilder& PlanBuilder::orderBy(
    const std::vector<std::string>& keys,
    bool isPartial) {
  auto [sortingKeys, sortingOrders] =
      parseOrderByClauses(keys, planNode_->outputType(), pool_);

  planNode_ = std::make_shared<core::OrderByNode>(
      nextPlanNodeId(), sortingKeys, sortingOrders, isPartial, planNode_);

  return *this;
}

PlanBuilder& PlanBuilder::topN(
    const std::vector<std::string>& keys,
    int32_t count,
    bool isPartial) {
  auto [sortingKeys, sortingOrders] =
      parseOrderByClauses(keys, planNode_->outputType(), pool_);
  planNode_ = std::make_shared<core::TopNNode>(
      nextPlanNodeId(),
      sortingKeys,
      sortingOrders,
      count,
      isPartial,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::limit(int32_t offset, int32_t count, bool isPartial) {
  planNode_ = std::make_shared<core::LimitNode>(
      nextPlanNodeId(), offset, count, isPartial, planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::enforceSingleRow() {
  planNode_ =
      std::make_shared<core::EnforceSingleRowNode>(nextPlanNodeId(), planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::assignUniqueId(
    const std::string& idName,
    const int32_t taskUniqueId) {
  planNode_ = std::make_shared<core::AssignUniqueIdNode>(
      nextPlanNodeId(), idName, taskUniqueId, planNode_);
  return *this;
}

namespace {
core::PartitionFunctionSpecPtr createPartitionFunctionSpec(
    const RowTypePtr& inputType,
    const std::vector<std::string>& keys) {
  if (keys.empty()) {
    return std::make_shared<core::GatherPartitionFunctionSpec>();
  } else {
    std::vector<column_index_t> keyIndices;
    keyIndices.reserve(keys.size());
    for (const auto& key : keys) {
      keyIndices.push_back(inputType->getChildIdx(key));
    }
    return std::make_shared<HashPartitionFunctionSpec>(
        inputType, std::move(keyIndices));
  }
}

RowTypePtr concat(const RowTypePtr& a, const RowTypePtr& b) {
  std::vector<std::string> names = a->names();
  std::vector<TypePtr> types = a->children();
  names.insert(names.end(), b->names().begin(), b->names().end());
  types.insert(types.end(), b->children().begin(), b->children().end());
  return ROW(std::move(names), std::move(types));
}

RowTypePtr extract(
    const RowTypePtr& type,
    const std::vector<std::string>& childNames) {
  std::vector<std::string> names = childNames;

  std::vector<TypePtr> types;
  types.reserve(childNames.size());
  for (const auto& name : childNames) {
    types.emplace_back(type->findChild(name));
  }
  return ROW(std::move(names), std::move(types));
}

// Rename columns in the given row type.
RowTypePtr rename(
    const RowTypePtr& type,
    const std::vector<std::string>& newNames) {
  VELOX_CHECK_EQ(
      type->size(),
      newNames.size(),
      "Number of types and new type names should be the same");
  std::vector<std::string> names{newNames};
  std::vector<TypePtr> types{type->children()};
  return ROW(std::move(names), std::move(types));
}

core::PlanNodePtr createLocalPartitionNode(
    const core::PlanNodeId& planNodeId,
    const std::vector<std::string>& keys,
    const std::vector<core::PlanNodePtr>& sources) {
  auto partitionFunctionFactory =
      createPartitionFunctionSpec(sources[0]->outputType(), keys);
  return std::make_shared<core::LocalPartitionNode>(
      planNodeId,
      keys.empty() ? core::LocalPartitionNode::Type::kGather
                   : core::LocalPartitionNode::Type::kRepartition,
      partitionFunctionFactory,
      sources);
}
} // namespace

PlanBuilder& PlanBuilder::partitionedOutput(
    const std::vector<std::string>& keys,
    int numPartitions,
    const std::vector<std::string>& outputLayout) {
  return partitionedOutput(keys, numPartitions, false, outputLayout);
}

PlanBuilder& PlanBuilder::partitionedOutput(
    const std::vector<std::string>& keys,
    int numPartitions,
    bool replicateNullsAndAny,
    const std::vector<std::string>& outputLayout) {
  return partitionedOutput(
      keys,
      numPartitions,
      replicateNullsAndAny,
      createPartitionFunctionSpec(planNode_->outputType(), keys),
      outputLayout);
}

PlanBuilder& PlanBuilder::partitionedOutput(
    const std::vector<std::string>& keys,
    int numPartitions,
    bool replicateNullsAndAny,
    core::PartitionFunctionSpecPtr partitionFunctionSpec,
    const std::vector<std::string>& outputLayout) {
  auto outputType = outputLayout.empty()
      ? planNode_->outputType()
      : extract(planNode_->outputType(), outputLayout);
  planNode_ = std::make_shared<core::PartitionedOutputNode>(
      nextPlanNodeId(),
      core::PartitionedOutputNode::Kind::kPartitioned,
      exprs(keys),
      numPartitions,
      replicateNullsAndAny,
      std::move(partitionFunctionSpec),
      outputType,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::partitionedOutputBroadcast(
    const std::vector<std::string>& outputLayout) {
  auto outputType = outputLayout.empty()
      ? planNode_->outputType()
      : extract(planNode_->outputType(), outputLayout);
  planNode_ = core::PartitionedOutputNode::broadcast(
      nextPlanNodeId(), 1, outputType, planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::localPartition(
    const std::vector<std::string>& keys,
    const std::vector<core::PlanNodePtr>& sources) {
  VELOX_CHECK_NULL(planNode_, "localPartition() must be the first call");
  planNode_ = createLocalPartitionNode(nextPlanNodeId(), keys, sources);
  return *this;
}

PlanBuilder& PlanBuilder::localPartition(const std::vector<std::string>& keys) {
  planNode_ = createLocalPartitionNode(nextPlanNodeId(), keys, {planNode_});
  return *this;
}

PlanBuilder& PlanBuilder::localPartitionByBucket(
    const std::shared_ptr<connector::hive::HiveBucketProperty>&
        bucketProperty) {
  std::vector<column_index_t> bucketChannels;
  for (const auto& bucketColumn : bucketProperty->bucketedBy()) {
    bucketChannels.push_back(
        planNode_->outputType()->getChildIdx(bucketColumn));
  }
  auto hivePartitionFunctionFactory =
      std::make_shared<HivePartitionFunctionSpec>(
          bucketProperty->bucketCount(),
          bucketChannels,
          std::vector<VectorPtr>{});
  planNode_ = std::make_shared<core::LocalPartitionNode>(
      nextPlanNodeId(),
      core::LocalPartitionNode::Type::kRepartition,
      std::move(hivePartitionFunctionFactory),
      std::vector<core::PlanNodePtr>{planNode_});
  return *this;
}

namespace {
core::PlanNodePtr createLocalPartitionRoundRobinNode(
    const core::PlanNodeId& planNodeId,
    const std::vector<core::PlanNodePtr>& sources) {
  return std::make_shared<core::LocalPartitionNode>(
      planNodeId,
      core::LocalPartitionNode::Type::kRepartition,
      std::make_shared<RoundRobinPartitionFunctionSpec>(),
      sources);
}
} // namespace

PlanBuilder& PlanBuilder::localPartitionRoundRobin(
    const std::vector<core::PlanNodePtr>& sources) {
  VELOX_CHECK_NULL(
      planNode_, "localPartitionRoundRobin() must be the first call");
  planNode_ = createLocalPartitionRoundRobinNode(nextPlanNodeId(), sources);
  return *this;
}

PlanBuilder& PlanBuilder::localPartitionRoundRobin() {
  planNode_ = createLocalPartitionRoundRobinNode(nextPlanNodeId(), {planNode_});
  return *this;
}

namespace {
class RoundRobinRowPartitionFunction : public core::PartitionFunction {
 public:
  explicit RoundRobinRowPartitionFunction(int numPartitions)
      : numPartitions_{numPartitions} {}

  std::optional<uint32_t> partition(
      const RowVector& input,
      std::vector<uint32_t>& partitions) override {
    auto size = input.size();
    partitions.resize(size);
    for (auto i = 0; i < size; ++i) {
      partitions[i] = counter_ % numPartitions_;
      ++counter_;
    }
    return std::nullopt;
  }

 private:
  const int numPartitions_;
  uint32_t counter_{0};
};

class RoundRobinRowPartitionFunctionSpec : public core::PartitionFunctionSpec {
 public:
  std::unique_ptr<core::PartitionFunction> create(
      int numPartitions) const override {
    return std::make_unique<RoundRobinRowPartitionFunction>(numPartitions);
  }

  std::string toString() const override {
    return "ROUND ROBIN ROW";
  }

  folly::dynamic serialize() const override {
    folly::dynamic obj = folly::dynamic::object;
    obj["name"] = fmt::format("RoundRobinRowPartitionFunctionSpec");
    return obj;
  }

  static core::PartitionFunctionSpecPtr deserialize(
      const folly::dynamic& /*obj*/,
      void* /*context*/) {
    return std::make_shared<RoundRobinRowPartitionFunctionSpec>();
  }
};
} // namespace

PlanBuilder& PlanBuilder::localPartitionRoundRobinRow() {
  planNode_ = std::make_shared<core::LocalPartitionNode>(
      nextPlanNodeId(),
      core::LocalPartitionNode::Type::kRepartition,
      std::make_shared<RoundRobinRowPartitionFunctionSpec>(),
      std::vector<core::PlanNodePtr>{planNode_});
  return *this;
}

PlanBuilder& PlanBuilder::hashJoin(
    const std::vector<std::string>& leftKeys,
    const std::vector<std::string>& rightKeys,
    const core::PlanNodePtr& build,
    const std::string& filter,
    const std::vector<std::string>& outputLayout,
    core::JoinType joinType,
    bool nullAware) {
  VELOX_CHECK_EQ(leftKeys.size(), rightKeys.size());

  auto leftType = planNode_->outputType();
  auto rightType = build->outputType();
  auto resultType = concat(leftType, rightType);
  core::TypedExprPtr filterExpr;
  if (!filter.empty()) {
    filterExpr = parseExpr(filter, resultType, options_, pool_);
  }

  RowTypePtr outputType;
  if (isLeftSemiProjectJoin(joinType) || isRightSemiProjectJoin(joinType)) {
    std::vector<std::string> names = outputLayout;

    // Last column in 'outputLayout' must be a boolean 'match'.
    std::vector<TypePtr> types;
    types.reserve(outputLayout.size());
    for (auto i = 0; i < outputLayout.size() - 1; ++i) {
      types.emplace_back(resultType->findChild(outputLayout[i]));
    }
    types.emplace_back(BOOLEAN());

    outputType = ROW(std::move(names), std::move(types));
  } else {
    outputType = extract(resultType, outputLayout);
  }

  auto leftKeyFields = fields(leftType, leftKeys);
  auto rightKeyFields = fields(rightType, rightKeys);

  planNode_ = std::make_shared<core::HashJoinNode>(
      nextPlanNodeId(),
      joinType,
      nullAware,
      leftKeyFields,
      rightKeyFields,
      std::move(filterExpr),
      std::move(planNode_),
      build,
      outputType);
  return *this;
}

PlanBuilder& PlanBuilder::mergeJoin(
    const std::vector<std::string>& leftKeys,
    const std::vector<std::string>& rightKeys,
    const core::PlanNodePtr& build,
    const std::string& filter,
    const std::vector<std::string>& outputLayout,
    core::JoinType joinType) {
  VELOX_CHECK_EQ(leftKeys.size(), rightKeys.size());

  auto leftType = planNode_->outputType();
  auto rightType = build->outputType();
  auto resultType = concat(leftType, rightType);
  core::TypedExprPtr filterExpr;
  if (!filter.empty()) {
    filterExpr = parseExpr(filter, resultType, options_, pool_);
  }
  auto outputType = extract(resultType, outputLayout);
  auto leftKeyFields = fields(leftType, leftKeys);
  auto rightKeyFields = fields(rightType, rightKeys);

  planNode_ = std::make_shared<core::MergeJoinNode>(
      nextPlanNodeId(),
      joinType,
      leftKeyFields,
      rightKeyFields,
      std::move(filterExpr),
      std::move(planNode_),
      build,
      outputType);
  return *this;
}

PlanBuilder& PlanBuilder::nestedLoopJoin(
    const core::PlanNodePtr& right,
    const std::vector<std::string>& outputLayout,
    core::JoinType joinType) {
  return nestedLoopJoin(right, "", outputLayout, joinType);
}

PlanBuilder& PlanBuilder::nestedLoopJoin(
    const core::PlanNodePtr& right,
    const std::string& joinCondition,
    const std::vector<std::string>& outputLayout,
    core::JoinType joinType) {
  auto resultType = concat(planNode_->outputType(), right->outputType());
  auto outputType = extract(resultType, outputLayout);

  core::TypedExprPtr joinConditionExpr{};
  if (!joinCondition.empty()) {
    joinConditionExpr = parseExpr(joinCondition, resultType, options_, pool_);
  }

  planNode_ = std::make_shared<core::NestedLoopJoinNode>(
      nextPlanNodeId(),
      joinType,
      std::move(joinConditionExpr),
      std::move(planNode_),
      right,
      outputType);
  return *this;
}

PlanBuilder& PlanBuilder::unnest(
    const std::vector<std::string>& replicateColumns,
    const std::vector<std::string>& unnestColumns,
    const std::optional<std::string>& ordinalColumn) {
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
      replicateFields;
  replicateFields.reserve(replicateColumns.size());
  for (const auto& name : replicateColumns) {
    replicateFields.emplace_back(field(name));
  }

  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>> unnestFields;
  unnestFields.reserve(unnestColumns.size());
  for (const auto& name : unnestColumns) {
    unnestFields.emplace_back(field(name));
  }

  std::vector<std::string> unnestNames;
  for (const auto& name : unnestColumns) {
    auto input = planNode_->outputType()->findChild(name);
    if (input->isArray()) {
      unnestNames.push_back(name + "_e");
    } else if (input->isMap()) {
      unnestNames.push_back(name + "_k");
      unnestNames.push_back(name + "_v");
    } else {
      VELOX_NYI(
          "Unsupported type of unnest variable. Expected ARRAY or MAP, but got {}.",
          input->toString());
    }
  }

  planNode_ = std::make_shared<core::UnnestNode>(
      nextPlanNodeId(),
      replicateFields,
      unnestFields,
      unnestNames,
      ordinalColumn,
      planNode_);
  return *this;
}

namespace {
std::string throwWindowFunctionDoesntExist(const std::string& name) {
  std::stringstream error;
  error << "Window function doesn't exist: " << name << ".";
  if (exec::windowFunctions().empty()) {
    error << " Registry of window functions is empty. "
             "Make sure to register some window functions.";
  }
  VELOX_USER_FAIL(error.str());
}

std::string throwWindowFunctionSignatureNotSupported(
    const std::string& name,
    const std::vector<TypePtr>& types,
    const std::vector<FunctionSignaturePtr>& signatures) {
  std::stringstream error;
  error << "Window function signature is not supported: "
        << toString(name, types)
        << ". Supported signatures: " << toString(signatures) << ".";
  VELOX_USER_FAIL(error.str());
}

TypePtr resolveWindowType(
    const std::string& windowFunctionName,
    const std::vector<TypePtr>& inputTypes,
    bool nullOnFailure) {
  if (auto signatures = exec::getWindowFunctionSignatures(windowFunctionName)) {
    for (const auto& signature : signatures.value()) {
      exec::SignatureBinder binder(*signature, inputTypes);
      if (binder.tryBind()) {
        return binder.tryResolveType(signature->returnType());
      }
    }

    if (nullOnFailure) {
      return nullptr;
    }
    throwWindowFunctionSignatureNotSupported(
        windowFunctionName, inputTypes, signatures.value());
  }

  if (nullOnFailure) {
    return nullptr;
  }
  throwWindowFunctionDoesntExist(windowFunctionName);
  return nullptr;
}

class WindowTypeResolver {
 public:
  explicit WindowTypeResolver()
      : previousHook_(core::Expressions::getResolverHook()) {
    core::Expressions::setTypeResolverHook(
        [&](const auto& inputs, const auto& expr, bool nullOnFailure) {
          return resolveType(inputs, expr, nullOnFailure);
        });
  }

  ~WindowTypeResolver() {
    core::Expressions::setTypeResolverHook(previousHook_);
  }

  void setResultType(const TypePtr& type) {
    resultType_ = type;
  }

 private:
  TypePtr resolveType(
      const std::vector<core::TypedExprPtr>& inputs,
      const std::shared_ptr<const core::CallExpr>& expr,
      bool nullOnFailure) const {
    if (resultType_) {
      return resultType_;
    }

    std::vector<TypePtr> types;
    for (auto& input : inputs) {
      types.push_back(input->type());
    }

    auto functionName = expr->getFunctionName();

    return resolveWindowType(functionName, types, nullOnFailure);
  }

  const core::Expressions::TypeResolverHook previousHook_;
  TypePtr resultType_;
};

const core::WindowNode::Frame createWindowFrame(
    const duckdb::IExprWindowFrame& windowFrame,
    const TypePtr& inputRow,
    memory::MemoryPool* pool) {
  core::WindowNode::Frame frame;
  frame.type = (windowFrame.type == duckdb::WindowType::kRows)
      ? core::WindowNode::WindowType::kRows
      : core::WindowNode::WindowType::kRange;

  auto boundTypeConversion =
      [](duckdb::BoundType boundType) -> core::WindowNode::BoundType {
    switch (boundType) {
      case duckdb::BoundType::kCurrentRow:
        return core::WindowNode::BoundType::kCurrentRow;
      case duckdb::BoundType::kFollowing:
        return core::WindowNode::BoundType::kFollowing;
      case duckdb::BoundType::kPreceding:
        return core::WindowNode::BoundType::kPreceding;
      case duckdb::BoundType::kUnboundedFollowing:
        return core::WindowNode::BoundType::kUnboundedFollowing;
      case duckdb::BoundType::kUnboundedPreceding:
        return core::WindowNode::BoundType::kUnboundedPreceding;
    }
    VELOX_UNREACHABLE();
  };
  frame.startType = boundTypeConversion(windowFrame.startType);
  frame.startValue = windowFrame.startValue
      ? core::Expressions::inferTypes(windowFrame.startValue, inputRow, pool)
      : nullptr;
  frame.endType = boundTypeConversion(windowFrame.endType);
  frame.endValue = windowFrame.endValue
      ? core::Expressions::inferTypes(windowFrame.endValue, inputRow, pool)
      : nullptr;
  return frame;
}

std::vector<core::FieldAccessTypedExprPtr> parsePartitionKeys(
    const duckdb::IExprWindowFunction& windowExpr,
    const std::string& windowString,
    const TypePtr& inputRow,
    memory::MemoryPool* pool) {
  std::vector<core::FieldAccessTypedExprPtr> partitionKeys;
  for (const auto& partitionKey : windowExpr.partitionBy) {
    auto typedExpr =
        core::Expressions::inferTypes(partitionKey, inputRow, pool);
    auto typedPartitionKey =
        std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(typedExpr);
    VELOX_CHECK_NOT_NULL(
        typedPartitionKey,
        "PARTITION BY clause must use a column name, not an expression: {}",
        windowString);
    partitionKeys.emplace_back(typedPartitionKey);
  }
  return partitionKeys;
}

std::pair<
    std::vector<core::FieldAccessTypedExprPtr>,
    std::vector<core::SortOrder>>
parseOrderByKeys(
    const duckdb::IExprWindowFunction& windowExpr,
    const std::string& windowString,
    const TypePtr& inputRow,
    memory::MemoryPool* pool) {
  std::vector<core::FieldAccessTypedExprPtr> sortingKeys;
  std::vector<core::SortOrder> sortingOrders;

  for (const auto& [untypedExpr, sortOrder] : windowExpr.orderBy) {
    auto typedExpr = core::Expressions::inferTypes(untypedExpr, inputRow, pool);
    auto sortingKey =
        std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(typedExpr);
    VELOX_CHECK_NOT_NULL(
        sortingKey,
        "ORDER BY clause must use a column name, not an expression: {}",
        windowString);
    sortingKeys.emplace_back(sortingKey);
    sortingOrders.emplace_back(sortOrder);
  }
  return {sortingKeys, sortingOrders};
}

bool equalFieldAccessTypedExprPtrList(
    const std::vector<core::FieldAccessTypedExprPtr>& lhs,
    const std::vector<core::FieldAccessTypedExprPtr>& rhs) {
  return std::equal(
      lhs.begin(),
      lhs.end(),
      rhs.begin(),
      [](const core::FieldAccessTypedExprPtr& e1,
         const core::FieldAccessTypedExprPtr& e2) {
        return e1->name() == e2->name();
      });
}

bool equalSortOrderList(
    const std::vector<core::SortOrder>& lhs,
    const std::vector<core::SortOrder>& rhs) {
  return std::equal(
      lhs.begin(),
      lhs.end(),
      rhs.begin(),
      [](const core::SortOrder& s1, const core::SortOrder& s2) {
        return s1.isAscending() == s2.isAscending() &&
            s1.isNullsFirst() == s2.isNullsFirst();
      });
}

} // namespace

PlanBuilder& PlanBuilder::window(
    const std::vector<std::string>& windowFunctions) {
  VELOX_CHECK_GT(
      windowFunctions.size(),
      0,
      "Window Node requires at least one window function.");

  std::vector<core::FieldAccessTypedExprPtr> partitionKeys;
  std::vector<core::FieldAccessTypedExprPtr> sortingKeys;
  std::vector<core::SortOrder> sortingOrders;
  std::vector<core::WindowNode::Function> windowNodeFunctions;
  std::vector<std::string> windowNames;

  bool first = true;
  auto inputType = planNode_->outputType();
  int i = 0;

  auto errorOnMismatch = [&](const std::string& windowString,
                             const std::string& mismatchTypeString) -> void {
    std::stringstream error;
    error << "Window function invocations " << windowString << " and "
          << windowFunctions[0] << " do not match " << mismatchTypeString
          << " clauses.";
    VELOX_USER_FAIL(error.str());
  };

  WindowTypeResolver windowResolver;
  facebook::velox::duckdb::ParseOptions options;
  options.parseIntegerAsBigint = options_.parseIntegerAsBigint;
  for (const auto& windowString : windowFunctions) {
    const auto& windowExpr = duckdb::parseWindowExpr(windowString, options);
    // All window function SQL strings in the list are expected to have the same
    // PARTITION BY and ORDER BY clauses. Validate this assumption.
    if (first) {
      partitionKeys =
          parsePartitionKeys(windowExpr, windowString, inputType, pool_);
      auto sortPair =
          parseOrderByKeys(windowExpr, windowString, inputType, pool_);
      sortingKeys = sortPair.first;
      sortingOrders = sortPair.second;
      first = false;
    } else {
      auto latestPartitionKeys =
          parsePartitionKeys(windowExpr, windowString, inputType, pool_);
      auto [latestSortingKeys, latestSortingOrders] =
          parseOrderByKeys(windowExpr, windowString, inputType, pool_);

      if (!equalFieldAccessTypedExprPtrList(
              partitionKeys, latestPartitionKeys)) {
        errorOnMismatch(windowString, "PARTITION BY");
      }

      if (!equalFieldAccessTypedExprPtrList(sortingKeys, latestSortingKeys)) {
        errorOnMismatch(windowString, "ORDER BY");
      }

      if (!equalSortOrderList(sortingOrders, latestSortingOrders)) {
        errorOnMismatch(windowString, "ORDER BY");
      }
    }

    auto windowCall = std::dynamic_pointer_cast<const core::CallTypedExpr>(
        core::Expressions::inferTypes(
            windowExpr.functionCall, planNode_->outputType(), pool_));
    windowNodeFunctions.push_back(
        {std::move(windowCall),
         createWindowFrame(windowExpr.frame, planNode_->outputType(), pool_),
         windowExpr.ignoreNulls});
    if (windowExpr.functionCall->alias().has_value()) {
      windowNames.push_back(windowExpr.functionCall->alias().value());
    } else {
      windowNames.push_back(fmt::format("w{}", i++));
    }
  }

  planNode_ = std::make_shared<core::WindowNode>(
      nextPlanNodeId(),
      partitionKeys,
      sortingKeys,
      sortingOrders,
      windowNames,
      windowNodeFunctions,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::rowNumber(
    const std::vector<std::string>& partitionKeys,
    std::optional<int32_t> limit,
    const bool generateRowNumber) {
  std::optional<std::string> rowNumberColumnName;
  if (generateRowNumber) {
    rowNumberColumnName = "row_number";
  }
  planNode_ = std::make_shared<core::RowNumberNode>(
      nextPlanNodeId(),
      fields(partitionKeys),
      rowNumberColumnName,
      limit,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::topNRowNumber(
    const std::vector<std::string>& partitionKeys,
    const std::vector<std::string>& sortingKeys,
    int32_t limit,
    bool generateRowNumber) {
  auto [sortingFields, sortingOrders] =
      parseOrderByClauses(sortingKeys, planNode_->outputType(), pool_);
  std::optional<std::string> rowNumberColumnName;
  if (generateRowNumber) {
    rowNumberColumnName = "row_number";
  }
  planNode_ = std::make_shared<core::TopNRowNumberNode>(
      nextPlanNodeId(),
      fields(partitionKeys),
      sortingFields,
      sortingOrders,
      rowNumberColumnName,
      limit,
      planNode_);
  return *this;
}

PlanBuilder& PlanBuilder::markDistinct(
    std::string markerKey,
    const std::vector<std::string>& distinctKeys) {
  planNode_ = std::make_shared<core::MarkDistinctNode>(
      nextPlanNodeId(),
      std::move(markerKey),
      fields(planNode_->outputType(), distinctKeys),

      planNode_);
  return *this;
}

core::PlanNodeId PlanBuilder::nextPlanNodeId() {
  return planNodeIdGenerator_->next();
}

// static
std::shared_ptr<const core::FieldAccessTypedExpr> PlanBuilder::field(
    const RowTypePtr& inputType,
    const std::string& name) {
  auto index = inputType->getChildIdx(name);
  return field(inputType, index);
}

// static
std::shared_ptr<const core::FieldAccessTypedExpr> PlanBuilder::field(
    const RowTypePtr& inputType,
    column_index_t index) {
  auto name = inputType->names()[index];
  auto type = inputType->childAt(index);
  return std::make_shared<core::FieldAccessTypedExpr>(type, name);
}

// static
std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
PlanBuilder::fields(
    const RowTypePtr& inputType,
    const std::vector<std::string>& names) {
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>> fields;
  for (const auto& name : names) {
    fields.push_back(field(inputType, name));
  }
  return fields;
}

// static
std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
PlanBuilder::fields(
    const RowTypePtr& inputType,
    const std::vector<column_index_t>& indices) {
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>> fields;
  for (auto& index : indices) {
    fields.push_back(field(inputType, index));
  }
  return fields;
}

std::shared_ptr<const core::FieldAccessTypedExpr> PlanBuilder::field(
    column_index_t index) {
  return field(planNode_->outputType(), index);
}

std::shared_ptr<const core::FieldAccessTypedExpr> PlanBuilder::field(
    const std::string& name) {
  return field(planNode_->outputType(), name);
}

std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
PlanBuilder::fields(const std::vector<std::string>& names) {
  return fields(planNode_->outputType(), names);
}

std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
PlanBuilder::fields(const std::vector<column_index_t>& indices) {
  return fields(planNode_->outputType(), indices);
}

std::vector<core::TypedExprPtr> PlanBuilder::exprs(
    const std::vector<std::string>& names) {
  auto flds = fields(planNode_->outputType(), names);
  std::vector<core::TypedExprPtr> expressions;
  expressions.reserve(flds.size());
  for (const auto& fld : flds) {
    expressions.emplace_back(
        std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(fld));
  }
  return expressions;
}

core::TypedExprPtr PlanBuilder::inferTypes(
    const std::shared_ptr<const core::IExpr>& untypedExpr) {
  return core::Expressions::inferTypes(
      untypedExpr, planNode_->outputType(), pool_);
}
} // namespace facebook::velox::exec::test
