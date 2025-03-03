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
#include "velox/exec/Window.h"
#include "velox/exec/OperatorUtils.h"
#include "velox/exec/SortWindowBuild.h"
#include "velox/exec/Task.h"

namespace facebook::velox::exec {

Window::Window(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const core::WindowNode>& windowNode)
    : Operator(
          driverCtx,
          windowNode->outputType(),
          operatorId,
          windowNode->id(),
          "Window"),
      numInputColumns_(windowNode->sources()[0]->outputType()->size()),
      windowBuild_(std::make_unique<SortWindowBuild>(windowNode, pool())),
      currentPartition_(nullptr),
      stringAllocator_(pool()) {
  auto inputType = windowNode->sources()[0]->outputType();
  createWindowFunctions(windowNode, inputType, driverCtx->queryConfig());

  createPeerAndFrameBuffers();
}

Window::WindowFrame Window::createWindowFrame(
    core::WindowNode::Frame frame,
    const RowTypePtr& inputType) {
  if (frame.type == core::WindowNode::WindowType::kRows) {
    auto frameBoundCheck = [&](const core::TypedExprPtr& frame) -> void {
      if (frame == nullptr) {
        return;
      }

      VELOX_USER_CHECK(
          frame->type() == INTEGER() || frame->type() == BIGINT(),
          "k frame bound must be INTEGER or BIGINT type");
    };
    frameBoundCheck(frame.startValue);
    frameBoundCheck(frame.endValue);
  }

  auto createFrameChannelArg =
      [&](const core::TypedExprPtr& frame) -> std::optional<FrameChannelArg> {
    // frame is nullptr for non (kPreceding or kFollowing) frames.
    if (frame == nullptr) {
      return std::nullopt;
    }
    auto frameChannel = exprToChannel(frame.get(), inputType);
    if (frameChannel == kConstantChannel) {
      auto constant =
          std::dynamic_pointer_cast<const core::ConstantTypedExpr>(frame)
              ->value();
      VELOX_CHECK(!constant.isNull(), "Window frame offset must not be null");
      auto value = VariantConverter::convert(constant, TypeKind::BIGINT)
                       .value<int64_t>();
      VELOX_USER_CHECK_GE(
          value, 0, "Window frame {} offset must not be negative", value);
      return std::make_optional(
          FrameChannelArg{kConstantChannel, nullptr, value});
    } else {
      return std::make_optional(FrameChannelArg{
          frameChannel,
          BaseVector::create(frame->type(), 0, pool()),
          std::nullopt});
    }
  };

  return WindowFrame(
      {frame.type,
       frame.startType,
       frame.endType,
       createFrameChannelArg(frame.startValue),
       createFrameChannelArg(frame.endValue)});
}

void Window::createWindowFunctions(
    const std::shared_ptr<const core::WindowNode>& windowNode,
    const RowTypePtr& inputType,
    const core::QueryConfig& config) {
  for (const auto& windowNodeFunction : windowNode->windowFunctions()) {
    std::vector<WindowFunctionArg> functionArgs;
    functionArgs.reserve(windowNodeFunction.functionCall->inputs().size());
    for (auto& arg : windowNodeFunction.functionCall->inputs()) {
      auto channel = exprToChannel(arg.get(), inputType);
      if (channel == kConstantChannel) {
        auto constantArg =
            std::dynamic_pointer_cast<const core::ConstantTypedExpr>(arg);
        functionArgs.push_back(
            {arg->type(), constantArg->toConstantVector(pool()), std::nullopt});
      } else {
        functionArgs.push_back({arg->type(), nullptr, channel});
      }
    }

    windowFunctions_.push_back(WindowFunction::create(
        windowNodeFunction.functionCall->name(),
        functionArgs,
        windowNodeFunction.functionCall->type(),
        windowNodeFunction.ignoreNulls,
        operatorCtx_->pool(),
        &stringAllocator_,
        config));

    windowFrames_.push_back(
        createWindowFrame(windowNodeFunction.frame, inputType));
  }
}

void Window::addInput(RowVectorPtr input) {
  windowBuild_->addInput(input);
  numRows_ += input->size();
}

void Window::createPeerAndFrameBuffers() {
  // TODO: This computation needs to be revised. It only takes into account
  // the input columns size. We need to also account for the output columns.
  numRowsPerOutput_ = outputBatchRows(windowBuild_->estimateRowSize());

  peerStartBuffer_ = AlignedBuffer::allocate<vector_size_t>(
      numRowsPerOutput_, operatorCtx_->pool());
  peerEndBuffer_ = AlignedBuffer::allocate<vector_size_t>(
      numRowsPerOutput_, operatorCtx_->pool());

  auto numFuncs = windowFunctions_.size();
  frameStartBuffers_.reserve(numFuncs);
  frameEndBuffers_.reserve(numFuncs);
  validFrames_.reserve(numFuncs);

  for (auto i = 0; i < numFuncs; i++) {
    BufferPtr frameStartBuffer = AlignedBuffer::allocate<vector_size_t>(
        numRowsPerOutput_, operatorCtx_->pool());
    BufferPtr frameEndBuffer = AlignedBuffer::allocate<vector_size_t>(
        numRowsPerOutput_, operatorCtx_->pool());
    frameStartBuffers_.push_back(frameStartBuffer);
    frameEndBuffers_.push_back(frameEndBuffer);
    validFrames_.push_back(SelectivityVector(numRowsPerOutput_));
  }
}

void Window::noMoreInput() {
  Operator::noMoreInput();
  // No data.
  if (numRows_ == 0) {
    return;
  }
  windowBuild_->noMoreInput();
}

void Window::callResetPartition() {
  partitionOffset_ = 0;
  peerStartRow_ = 0;
  peerEndRow_ = 0;
  currentPartition_ = nullptr;
  if (windowBuild_->hasNextPartition()) {
    currentPartition_ = windowBuild_->nextPartition();
    for (int i = 0; i < windowFunctions_.size(); i++) {
      windowFunctions_[i]->resetPartition(currentPartition_.get());
    }
  }
}

namespace {

template <typename T>
void updateKRowsOffsetsColumn(
    bool isKPreceding,
    const VectorPtr& value,
    vector_size_t startRow,
    vector_size_t numRows,
    vector_size_t* rawFrameBounds) {
  auto offsets = value->values()->as<T>();
  for (auto i = 0; i < numRows; i++) {
    VELOX_USER_CHECK(
        !value->isNullAt(i), "Window frame offset must not be null");
    VELOX_USER_CHECK_GE(
        offsets[i],
        0,
        "Window frame {} offset must not be negative",
        offsets[i]);
  }

  // Preceding involves subtracting from the current position, while following
  // moves ahead.
  int precedingFactor = isKPreceding ? -1 : 1;
  for (auto i = 0; i < numRows; i++) {
    rawFrameBounds[i] =
        (startRow + i) + vector_size_t(precedingFactor * offsets[i]);
  }
}

}; // namespace

void Window::updateKRowsFrameBounds(
    bool isKPreceding,
    const FrameChannelArg& frameArg,
    vector_size_t startRow,
    vector_size_t numRows,
    vector_size_t* rawFrameBounds) {
  if (frameArg.index == kConstantChannel) {
    auto constantOffset = frameArg.constant.value();
    auto startValue =
        startRow + (isKPreceding ? -constantOffset : constantOffset);
    std::iota(rawFrameBounds, rawFrameBounds + numRows, startValue);
  } else {
    currentPartition_->extractColumn(
        frameArg.index, partitionOffset_, numRows, 0, frameArg.value);
    if (frameArg.value->typeKind() == TypeKind::INTEGER) {
      updateKRowsOffsetsColumn<int32_t>(
          isKPreceding, frameArg.value, startRow, numRows, rawFrameBounds);
    } else {
      updateKRowsOffsetsColumn<int64_t>(
          isKPreceding, frameArg.value, startRow, numRows, rawFrameBounds);
    }
  }
}

void Window::updateFrameBounds(
    const WindowFrame& windowFrame,
    const bool isStartBound,
    const vector_size_t startRow,
    const vector_size_t numRows,
    const vector_size_t* rawPeerStarts,
    const vector_size_t* rawPeerEnds,
    vector_size_t* rawFrameBounds) {
  auto windowType = windowFrame.type;
  auto boundType = isStartBound ? windowFrame.startType : windowFrame.endType;
  auto frameArg = isStartBound ? windowFrame.start : windowFrame.end;

  switch (boundType) {
    case core::WindowNode::BoundType::kUnboundedPreceding:
      std::fill_n(rawFrameBounds, numRows, 0);
      break;
    case core::WindowNode::BoundType::kUnboundedFollowing:
      std::fill_n(rawFrameBounds, numRows, currentPartition_->numRows() - 1);
      break;
    case core::WindowNode::BoundType::kCurrentRow: {
      if (windowType == core::WindowNode::WindowType::kRange) {
        const vector_size_t* rawPeerBuffer =
            isStartBound ? rawPeerStarts : rawPeerEnds;
        std::copy(rawPeerBuffer, rawPeerBuffer + numRows, rawFrameBounds);
      } else {
        // Fills the frameBound buffer with increasing value of row indices
        // (corresponding to CURRENT ROW) from the startRow of the current
        // output buffer.
        std::iota(rawFrameBounds, rawFrameBounds + numRows, startRow);
      }
      break;
    }
    case core::WindowNode::BoundType::kPreceding: {
      if (windowType == core::WindowNode::WindowType::kRows) {
        updateKRowsFrameBounds(
            true, frameArg.value(), startRow, numRows, rawFrameBounds);
      } else {
        VELOX_NYI("k preceding frame is only supported in ROWS mode");
      }
      break;
    }
    case core::WindowNode::BoundType::kFollowing: {
      if (windowType == core::WindowNode::WindowType::kRows) {
        updateKRowsFrameBounds(
            false, frameArg.value(), startRow, numRows, rawFrameBounds);
      } else {
        VELOX_NYI("k following frame is only supported in ROWS mode");
      }
      break;
    }
    default:
      VELOX_USER_FAIL("Invalid frame bound type");
  }
}

namespace {
// Frame end points are always expected to go from frameStart to frameEnd
// rows in increasing row numbers in the partition. k rows/range frames could
// potentially violate this.
// This function identifies the rows that violate the framing requirements
// and sets bits in the validFrames SelectivityVector for usage in the
// WindowFunction subsequently.
void computeValidFrames(
    vector_size_t lastRow,
    vector_size_t numRows,
    vector_size_t* rawFrameStarts,
    vector_size_t* rawFrameEnds,
    SelectivityVector& validFrames) {
  auto frameStart = 0;
  auto frameEnd = 0;

  for (auto i = 0; i < numRows; i++) {
    frameStart = rawFrameStarts[i];
    frameEnd = rawFrameEnds[i];
    // All valid frames require frameStart <= frameEnd to define the frame rows.
    // Also, frameEnd >= 0, so that the frameEnd doesn't fall before the
    // partition. And frameStart <= lastRow so that the frameStart doesn't fall
    // after the partition rows.
    if (frameStart <= frameEnd && frameEnd >= 0 && frameStart <= lastRow) {
      rawFrameStarts[i] = std::max(frameStart, 0);
      rawFrameEnds[i] = std::min(frameEnd, lastRow);
    } else {
      validFrames.setValid(i, false);
    }
  }
  validFrames.updateBounds();
}

}; // namespace

void Window::computePeerAndFrameBuffers(
    vector_size_t startRow,
    vector_size_t endRow) {
  vector_size_t numRows = endRow - startRow;
  vector_size_t numFuncs = windowFunctions_.size();

  // Size buffers for the call to WindowFunction::apply.
  auto bufferSize = numRows * sizeof(vector_size_t);
  peerStartBuffer_->setSize(bufferSize);
  peerEndBuffer_->setSize(bufferSize);
  auto rawPeerStarts = peerStartBuffer_->asMutable<vector_size_t>();
  auto rawPeerEnds = peerEndBuffer_->asMutable<vector_size_t>();

  std::vector<vector_size_t*> rawFrameStarts;
  std::vector<vector_size_t*> rawFrameEnds;
  rawFrameStarts.reserve(numFuncs);
  rawFrameEnds.reserve(numFuncs);
  for (auto w = 0; w < numFuncs; w++) {
    frameStartBuffers_[w]->setSize(bufferSize);
    frameEndBuffers_[w]->setSize(bufferSize);

    auto rawFrameStart = frameStartBuffers_[w]->asMutable<vector_size_t>();
    auto rawFrameEnd = frameEndBuffers_[w]->asMutable<vector_size_t>();
    rawFrameStarts.push_back(rawFrameStart);
    rawFrameEnds.push_back(rawFrameEnd);
  }

  std::tie(peerStartRow_, peerEndRow_) = currentPartition_->computePeerBuffers(
      startRow, endRow, peerStartRow_, peerEndRow_, rawPeerStarts, rawPeerEnds);

  for (auto i = 0; i < numFuncs; i++) {
    const auto& windowFrame = windowFrames_[i];
    // Default all rows to have validFrames. The invalidity of frames is only
    // computed for k rows/range frames at a later point.
    validFrames_[i].resizeFill(numRows, true);
    updateFrameBounds(
        windowFrame,
        true,
        startRow,
        numRows,
        rawPeerStarts,
        rawPeerEnds,
        rawFrameStarts[i]);
    updateFrameBounds(
        windowFrame,
        false,
        startRow,
        numRows,
        rawPeerStarts,
        rawPeerEnds,
        rawFrameEnds[i]);
    if (windowFrames_[i].start || windowFrames_[i].end) {
      // k preceding and k following bounds can be problematic. They can
      // go over the partition limits or result in empty frames. Fix the
      // frame boundaries and compute the validFrames SelectivityVector
      // for these cases. Not all functions care about validFrames viz.
      // Ranking functions do not care about frames. So the function decides
      // further what to do with empty frames.
      computeValidFrames(
          currentPartition_->numRows() - 1,
          numRows,
          rawFrameStarts[i],
          rawFrameEnds[i],
          validFrames_[i]);
    }
  }
}

void Window::getInputColumns(
    vector_size_t startRow,
    vector_size_t endRow,
    vector_size_t resultOffset,
    const RowVectorPtr& result) {
  auto numRows = endRow - startRow;
  for (int i = 0; i < numInputColumns_; ++i) {
    currentPartition_->extractColumn(
        i, partitionOffset_, numRows, resultOffset, result->childAt(i));
  }
}

void Window::callApplyForPartitionRows(
    vector_size_t startRow,
    vector_size_t endRow,
    vector_size_t resultOffset,
    const RowVectorPtr& result) {
  getInputColumns(startRow, endRow, resultOffset, result);

  computePeerAndFrameBuffers(startRow, endRow);
  vector_size_t numFuncs = windowFunctions_.size();
  for (auto w = 0; w < numFuncs; w++) {
    windowFunctions_[w]->apply(
        peerStartBuffer_,
        peerEndBuffer_,
        frameStartBuffers_[w],
        frameEndBuffers_[w],
        validFrames_[w],
        resultOffset,
        result->childAt(numInputColumns_ + w));
  }

  vector_size_t numRows = endRow - startRow;
  numProcessedRows_ += numRows;
  partitionOffset_ += numRows;
}

void Window::callApplyLoop(
    vector_size_t numOutputRows,
    const RowVectorPtr& result) {
  // Compute outputs by traversing as many partitions as possible. This
  // logic takes care of partial partitions output also.
  vector_size_t resultIndex = 0;
  vector_size_t numOutputRowsLeft = numOutputRows;

  // This function requires that the currentPartition_ is available for output.
  VELOX_DCHECK_NOT_NULL(currentPartition_);
  while (numOutputRowsLeft > 0) {
    auto rowsForCurrentPartition =
        currentPartition_->numRows() - partitionOffset_;
    if (rowsForCurrentPartition <= numOutputRowsLeft) {
      // Current partition can fit completely in the output buffer.
      // So output all its rows.
      callApplyForPartitionRows(
          partitionOffset_,
          partitionOffset_ + rowsForCurrentPartition,
          resultIndex,
          result);
      resultIndex += rowsForCurrentPartition;
      numOutputRowsLeft -= rowsForCurrentPartition;
      callResetPartition();
      if (!currentPartition_) {
        // The WindowBuild doesn't have any more partitions to process right
        // now. So break until the next getOutput call.
        break;
      }
    } else {
      // Current partition can fit only partially in the output buffer.
      // Call apply for the rows that can fit in the buffer and break from
      // outputting.
      callApplyForPartitionRows(
          partitionOffset_,
          partitionOffset_ + numOutputRowsLeft,
          resultIndex,
          result);
      numOutputRowsLeft = 0;
      break;
    }
  }
}

RowVectorPtr Window::getOutput() {
  if (numRows_ == 0) {
    return nullptr;
  }

  auto numRowsLeft = numRows_ - numProcessedRows_;
  if (numRowsLeft == 0) {
    return nullptr;
  }

  if (!currentPartition_) {
    callResetPartition();
    if (!currentPartition_) {
      // WindowBuild doesn't have a partition to output.
      return nullptr;
    }
  }

  auto numOutputRows = std::min(numRowsPerOutput_, numRowsLeft);
  auto result = std::dynamic_pointer_cast<RowVector>(
      BaseVector::create(outputType_, numOutputRows, operatorCtx_->pool()));

  for (int i = numInputColumns_; i < outputType_->size(); i++) {
    auto output = BaseVector::create(
        outputType_->childAt(i), numOutputRows, operatorCtx_->pool());
    result->childAt(i) = output;
  }

  // Compute the output values of window functions.
  callApplyLoop(numOutputRows, result);
  return result;
}

} // namespace facebook::velox::exec
