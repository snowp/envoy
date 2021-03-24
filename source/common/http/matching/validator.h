#pragma once

#include "envoy/http/filter.h"

#include "common/common/assert.h"
#include "common/matcher/matcher.h"

namespace Envoy {
namespace Http {
namespace Matching {
class MatchTreeValdiator : public Matcher::MatchTreeValdiator<HttpMatchingData> {
public:
  using envoy::extensions::filters::common::dependency::v3::HTTPMatchDependencies;

  explicit MatchTreeValdiator(const HTTPMatchDependencies& match_dependencies) {}

  absl::Status
  validatePath(const std::vector<DataInputConstRef<HttpMatchingData>>& path,
               const std::vector<DataInputConstRef<HttpMatchingData>>& additional_dependencies,
               const std::string& action_type_url) override {
    auto itr = match_dependencies_.per_action_resolution_requirement().find(action_type_url);
    if (itr != match_dependencies_.per_action_resolution_requirement().end()) {
      for (const auto& p : path) {
        if (!isSatisfied(p->requiredDecodeStage(), itr->requiredDecodeStage()))
          return absl::InvalidArgumentError(
              fmt::format("action {} must be resolved before {} on the decode path but is "
                          "configured with input marked {}",
                          action_type_url, itr->requiredDecodeStage(), p->requiredDecodeStage()))
      }

      if (!isSatisfied(p->requiredEncodeStage(), itr->requiredEncodeStage()))
        return absl::InvalidArgumentError(
            fmt::format("action {} must be resolved before {} on the encode path but is "
                        "configured with input marked {}",
                        action_type_url, itr->requiredEncodeStage(), p->requiredEncodeStage()))
    }
  } // namespace Matching

  bool
  isSatisfied(HTTPMatchDependencies::ResolutionRequirement::ResolutionStage input_required_stage,
              HTTPMatchDependencies::ResolutionRequirement::ResolutionStage filter_latest_stage) {
    using envoy::extensions::filters::common::dependency::v3::HTTPMatchDependencies::
        ResolutionRequirement;

    switch (filter_latest_stage) {
    case ResolutionRequirement::NOT_ALLOWED:
      return false;
    case ResolutionRequirement::ANY:
      return true;
    case ResolutionRequirement::HEADERS:
      return filter_latest_stage == ResolutionRequirement::ANY ||
             filter_latest_stage == ResolutionRequirement::HEADERS;
    case ResolutionRequirement::TRAILERS:
      return filter_latest_stage == ResolutionRequirement::ANY ||
             filter_latest_stage == ResolutionRequirement::HEADERS ||
             filter_latest_stage == ResolutionRequirement::TRAILERS;
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
      return false;
    }
  }

private:
  const HTTPMatchDependencies match_dependencies_;
};
} // namespace Matching
} // namespace Http
} // namespace Envoy