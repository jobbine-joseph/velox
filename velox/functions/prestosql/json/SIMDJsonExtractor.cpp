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

#include "velox/functions/prestosql/json/SIMDJsonExtractor.h"

namespace facebook::velox::functions::detail {
/* static */ SIMDJsonExtractor& SIMDJsonExtractor::getInstance(
    folly::StringPiece path) {
  // Cache tokenize operations in JsonExtractor across invocations in the same
  // thread for the same JsonPath.
  thread_local static std::
      unordered_map<std::string, std::shared_ptr<SIMDJsonExtractor>>
          extractorCache;
  // Pre-process
  auto trimmedPath = folly::trimWhitespace(path).str();

  std::shared_ptr<SIMDJsonExtractor> op;
  if (extractorCache.count(trimmedPath)) {
    return *extractorCache.at(trimmedPath);
  }

  if (extractorCache.size() == kMaxCacheSize) {
    // TODO: Blindly evict the first one, use better policy
    extractorCache.erase(extractorCache.begin());
  }

  auto it =
      extractorCache.emplace(trimmedPath, new SIMDJsonExtractor(trimmedPath));
  return *it.first->second;
}

simdjson::ondemand::document SIMDJsonExtractor::parse(
    const simdjson::padded_string& json) {
  thread_local static simdjson::ondemand::parser parser;
  return parser.iterate(json);
}

bool SIMDJsonExtractor::tokenize(const std::string& path) {
  thread_local static JsonPathTokenizer tokenizer;

  if (path.empty()) {
    return false;
  }

  if (!tokenizer.reset(path)) {
    return false;
  }

  while (tokenizer.hasNext()) {
    if (auto token = tokenizer.getNext()) {
      tokens_.push_back(token.value());
    } else {
      tokens_.clear();
      return false;
    }
  }

  return true;
}

void extractObject(
    simdjson::ondemand::value& jsonObj,
    const std::string& key,
    std::optional<simdjson::ondemand::value>& ret) {
  for (auto field : jsonObj.get_object()) {
    if (field.unescaped_key().value() == key) {
      ret.emplace(field.value());
      return;
    }
  }
}

void extractArray(
    simdjson::ondemand::value& jsonValue,
    const std::string& index,
    std::optional<simdjson::ondemand::value>& ret) {
  auto jsonArray = jsonValue.get_array();
  auto rv = folly::tryTo<int32_t>(index);
  if (rv.hasValue()) {
    auto val = jsonArray.at(rv.value());
    if (!val.error()) {
      ret.emplace(std::move(val));
    }
  }
}
} // namespace facebook::velox::functions::detail
