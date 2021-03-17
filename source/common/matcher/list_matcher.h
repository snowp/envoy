#pragma once

#include "envoy/matcher/matcher.h"

#include "common/matcher/field_matcher.h"
#include <memory>

namespace Envoy {
namespace Matcher {

/**
 * A match tree that iterates over a list of matchers to find the first one that matches. If one
 * does, the MatchResult will be the one specified by the individual matcher.
 */
template <class DataType> class ListMatcher : public MatchTree<DataType> {
public:
  class Builder {
  public:
    void addMatcher(FieldMatcherPtr<DataType>&& matcher, OnMatch<DataType> action) {
      // The OnMatch must be constructed *after* all the children in order to properly account for
      // all dependent inputs.
      ASSERT(!on_no_match_);

      matchers_.push_back({std::move(matcher), std::move(action)});
    }
    void setOnNoMatch(absl::optional<OnMatch<DataType>>&& on_no_match) {
      ASSERT(!on_no_match_);

      if (on_no_match) {
        on_no_match_ = std::make_unique<OnMatch<DataType>>(*on_no_match);
      }
    }

    std::unique_ptr<ListMatcher<DataType>> build() {
      return std::unique_ptr<ListMatcher<DataType>>(new ListMatcher<DataType>(*this));
    }

  private:
    std::unique_ptr<OnMatch<DataType>> on_no_match_;
    std::vector<std::pair<FieldMatcherPtr<DataType>, OnMatch<DataType>>> matchers_;

    friend ListMatcher<DataType>;
  };

  typename MatchTree<DataType>::MatchResult match(const DataType& matching_data) override {
    for (const auto& matcher : matchers_) {
      const auto maybe_match = matcher.first->match(matching_data);

      // One of the matchers don't have enough information, bail on evaluating the match.
      if (maybe_match.match_state_ == MatchState::UnableToMatch) {
        return {MatchState::UnableToMatch, {}};
      }

      if (maybe_match.result()) {
        return {MatchState::MatchComplete, matcher.second};
      }
    }

    return {MatchState::MatchComplete, on_no_match_};
  }

private:
  explicit ListMatcher(Builder& builder)
      : on_no_match_(builder.on_no_match_ ? absl::make_optional(std::move(*builder.on_no_match_))
                                          : absl::nullopt),
        matchers_(std::move(builder.matchers_)) {}

  const absl::optional<OnMatch<DataType>> on_no_match_;
  const std::vector<std::pair<FieldMatcherPtr<DataType>, OnMatch<DataType>>> matchers_;
};

} // namespace Matcher
} // namespace Envoy