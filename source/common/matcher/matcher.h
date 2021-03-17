#pragma once

#include <memory>
#include <variant>

#include "envoy/config/common/matcher/v3/matcher.pb.h"
#include "envoy/config/core/v3/extension.pb.h"
#include "envoy/config/typed_config.h"
#include "envoy/matcher/matcher.h"

#include "common/common/assert.h"
#include "common/config/utility.h"
#include "common/matcher/exact_map_matcher.h"
#include "common/matcher/field_matcher.h"
#include "common/matcher/list_matcher.h"
#include "common/matcher/value_input_matcher.h"

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

namespace Envoy {
namespace Matcher {

template <class ProtoType> class ActionBase : public Action {
public:
  ActionBase() : type_name_(ProtoType().GetTypeName()) {}

  absl::string_view typeUrl() const override { return type_name_; }

private:
  const std::string type_name_;
};

struct MaybeMatchResult {
  const ActionPtr result_;
  const MatchState match_state_;
};

// TODO(snowp): Make this a class that tracks the progress to speed up subsequent traversals.
template <class DataType>
static inline MaybeMatchResult evaluateMatch(MatchTree<DataType>& match_tree,
                                             const DataType& data) {
  const auto result = match_tree.match(data);
  if (result.match_state_ == MatchState::UnableToMatch) {
    return MaybeMatchResult{nullptr, MatchState::UnableToMatch};
  }

  if (!result.on_match_) {
    return {nullptr, MatchState::MatchComplete};
  }

  if (result.on_match_->matcher_) {
    return evaluateMatch(*result.on_match_->matcher_, data);
  }

  return MaybeMatchResult{result.on_match_->action_cb_(), MatchState::MatchComplete};
}

template <class DataType> class MatchTreeValidator {
public:
  virtual ~MatchTreeValidator() = default;

  virtual absl::Status
  validatePath(const std::vector<DataInputConstRef<DataType>>& path,
               const std::vector<DataInputConstRef<DataType>>& additional_dependencies,
               const std::string& action_type_url) PURE;
};

template <class DataType> class NullMatchTreeValidator : public MatchTreeValidator<DataType> {
  absl::Status validatePath(const std::vector<DataInputConstRef<DataType>>&,
                            const std::vector<DataInputConstRef<DataType>>&,
                            const std::string&) override {
    return absl::OkStatus();
  }
};

/**
 * Recursively constructs a MatchTree from a protobuf configuration.
 */
template <class DataType> class MatchTreeFactory {
public:
  MatchTreeFactory(Server::Configuration::FactoryContext& factory_context,
                   MatchTreeValidator<DataType>& validator)
      : factory_context_(factory_context), validator_(validator) {}

  MatchTreeSharedPtr<DataType> create(const envoy::config::common::matcher::v3::Matcher& config) {
    return createInternal(config);
  }

private:
  struct PathContext {
    PathContext(MatchTreeFactory<DataType>& factory, const DataInput<DataType>& input)
        : factory_(factory) {
      factory_.current_path_.push_back(std::cref(input));
    }
    ~PathContext() { factory_.current_path_.pop_back(); }

    MatchTreeFactory<DataType>& factory_;
  };

  template <class T>
  using PathTrackingContext = std::pair<T, std::vector<std::unique_ptr<PathContext>>>;

  struct SubtreeTracker {
    SubtreeTracker(MatchTreeFactory<DataType>& factory) : factory_(factory), start_itr_(factory.subtree_tracker_.end()) {
    }

    std::vector<DataInputConstRef<DataType>> subtree() {
      if (start_itr_ == factory_.subtree_tracker_.end()) {
        return {};
      }

      return std::vector<DataInputConstRef<DataType>>(start_itr_, factory_.subtree_tracker_.end());
    }

    MatchTreeFactory<DataType>& factory_;
    typename std::vector<DataInputConstRef<DataType>>::iterator start_itr_;
  };

  MatchTreeSharedPtr<DataType>
  createInternal(const envoy::config::common::matcher::v3::Matcher& config) {
    switch (config.matcher_type_case()) {
    case envoy::config::common::matcher::v3::Matcher::kMatcherTree: {
      return createTreeMatcher(config);
    }
    case envoy::config::common::matcher::v3::Matcher::kMatcherList: {
      return createListMatcher(config);
    }
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
      return nullptr;
    }
  }

  MatchTreeSharedPtr<DataType>
  createListMatcher(const envoy::config::common::matcher::v3::Matcher& config) {
    typename ListMatcher<DataType>::Builder builder;

    SubtreeTracker subtree_tracker;
    std::vector<DataInputConstRef<DataType>> subtree_inputs;
    for (const auto& matcher : config.matcher_list().matchers()) {
      auto field_matcher_context = createFieldMatcher(matcher.predicate());

      auto subtree_tracking_on_match = createOnMatch(matcher.on_match(), {});
      builder.addMatcher(std::move(field_matcher_context.first.first),
                         *subtree_tracking_on_match.first);

      subtree_tracker_.reserve(subtree_tracker_.size() + subtree_tracking_on_match.second.size());
      subtree_tracker_.insert(subtree_tracker_.end(), subtree_tracking_on_match.second.begin(),
                              subtree_tracking_on_match.second.end());
    }

    builder.setOnNoMatch(createOnMatch(config.on_no_match(), subtree_tracker.subtree()).first);

    return builder.build();
  }

  PathTrackingContext<FieldMatcherPtr<DataType>> createFieldMatcher(
      const envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate& field_predicate) {
    std::vector<DataInputConstRef<DataType>> subtree_inputs;
    switch (field_predicate.match_type_case()) {
    case (envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::kSinglePredicate): {
      auto data_input_context = createDataInput(field_predicate.single_predicate().input());
      subtree_tracker_.emplace_back(*data_input_context.first);

      auto field_matcher = std::make_unique<SingleFieldMatcher<DataType>>(
          std::move(data_input_context.first),
          createInputMatcher(field_predicate.single_predicate()));

      return {std::move(field_matcher), std::move(data_input_context.second)};
    }
    case (envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::kOrMatcher): {
      std::vector<std::unique_ptr<PathContext>> contexts;
      std::vector<FieldMatcherPtr<DataType>> sub_matchers;
      for (const auto& predicate : field_predicate.or_matcher().predicate()) {
        auto field_matcher_context = createFieldMatcher(predicate);

        subtree_tracker_.reserve(subtree_tracker_.size() + field_matcher_context.second.size());
        subtree_tracker_.insert(subtree_tracker_.end(), field_matcher_context.second.begin(),
                                field_matcher_context.second.end());

        for (auto&& c : field_matcher_context.first.second) {
          contexts.emplace_back(std::move(c));
        }

        sub_matchers.emplace_back(std::move(field_matcher_context.first.first));
      }

      return {std::make_unique<AnyFieldMatcher<DataType>>(std::move(sub_matchers)),
              std::move(contexts)};
    }
    case (envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::kAndMatcher): {
      std::vector<std::unique_ptr<PathContext>> contexts;
      std::vector<FieldMatcherPtr<DataType>> sub_matchers;
      for (const auto& predicate : field_predicate.and_matcher().predicate()) {
        auto field_matcher_context = createFieldMatcher(predicate);

        subtree_tracker_.reserve(subtree_tracker_.size() + field_matcher_context.second.size());
        subtree_tracker_.insert(subtree_tracker_.end(), field_matcher_context.second.begin(),
                                field_matcher_context.second.end());

        for (auto&& c : field_matcher_context.first.second) {
          contexts.emplace_back(std::move(c));
        }

        sub_matchers.emplace_back(std::move(field_matcher_context.first.first));
      }

      return {std::make_unique<AllFieldMatcher<DataType>>(std::move(sub_matchers)),
              std::move(contexts)};
    }
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }
  }

  MatchTreeSharedPtr<DataType>
  createTreeMatcher(const envoy::config::common::matcher::v3::Matcher& matcher) {
    switch (matcher.matcher_tree().tree_type_case()) {
    case envoy::config::common::matcher::v3::Matcher_MatcherTree::kExactMatchMap: {
      auto data_input_context = createDataInput(matcher.matcher_tree().input());

      typename ExactMapMatcher<DataType>::Builder builder(std::move(data_input_context.first));

      for (const auto& children : matcher.matcher_tree().exact_match_map().map()) {
        auto on_match = createOnMatch(children.second, {});

        builder.addChild(children.first, std::move(on_match));
      }

      builder.setOnNoMatch(createOnMatch(matcher.on_no_match()).first);

      return builder.build();
    }
    case envoy::config::common::matcher::v3::Matcher_MatcherTree::kPrefixMatchMap:
      NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
    case envoy::config::common::matcher::v3::Matcher_MatcherTree::kCustomMatch:
      NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }
  }

  absl::optional<OnMatch<DataType>>
  createOnMatch(const envoy::config::common::matcher::v3::Matcher::OnMatch& on_match, const std::vector<DataInputConstRef<DataType>>& subtree) {
    if (on_match.has_matcher()) {
      ENVOY_LOG_MISC(trace, "creating recursive on match matcher");

      auto subtree_with_context = createInternal(on_match.matcher());
      return OnMatch<DataType>{{}, createInternal(on_match.matcher())};
    } else if (on_match.has_action()) {
      auto& factory = Config::Utility::getAndCheckFactory<ActionFactory>(on_match.action());
      ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
          on_match.action().typed_config(), factory_context_.messageValidationVisitor(), factory);
      auto action = factory.createActionFactoryCb(*message, "", factory_context_);

      auto status =
          validator_.validatePath(current_path_, subtree, factory.configType());
      if (!status.ok()) {
        throw EnvoyException(fmt::format("match tree validation failed: {}", status));
      }

      ENVOY_LOG_MISC(trace, "created on match with action type {}",
                     on_match.action().typed_config().type_url());
      return OnMatch<DataType>{factory.createActionFactoryCb(*message, "", factory_context_), {}};
    }

    ENVOY_LOG_MISC(trace, "noop on_match");

    return absl::nullopt;
  }

  PathTrackingContext<DataInputPtr<DataType>>
  createDataInput(const envoy::config::core::v3::TypedExtensionConfig& config) {
    auto& factory = Config::Utility::getAndCheckFactory<DataInputFactory<DataType>>(config);
    ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
        config.typed_config(), factory_context_.messageValidationVisitor(), factory);
    auto input = factory.createDataInput(*message, factory_context_);

    auto context = std::make_unique<PathContext>(*this, *input);
    PathTrackingContext<DataInputPtr<DataType>> creation_context;
    creation_context.first = std::move(input);
    creation_context.second.emplace_back(std::move(context));
    return creation_context;
  }

  InputMatcherPtr createInputMatcher(
      const envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::SinglePredicate&
          predicate) {
    switch (predicate.matcher_case()) {
    case envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::SinglePredicate::
        kValueMatch:
      return std::make_unique<StringInputMatcher>(predicate.value_match());
    case envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::SinglePredicate::
        kCustomMatch: {
      auto& factory =
          Config::Utility::getAndCheckFactory<InputMatcherFactory>(predicate.custom_match());
      ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
          predicate.custom_match().typed_config(), factory_context_.messageValidationVisitor(),
          factory);
      return factory.createInputMatcher(*message, factory_context_);
    }
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }
  }

  std::vector<DataInputConstRef<DataType>> current_path_;
  std::vector<DataInputConstRef<DataType>> subtree_tracker_;
  Server::Configuration::FactoryContext& factory_context_;
  MatchTreeValidator<DataType>& validator_;
};
} // namespace Matcher
} // namespace Envoy
