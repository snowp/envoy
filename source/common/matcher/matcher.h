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

  virtual absl::Status validatePath(const std::vector<DataInputConstRef<DataType>>& path,
                                    const std::string& action_type_url) PURE;
};

template <class DataType> class NullMatchTreeValidator : public MatchTreeValidator<DataType> {
  absl::Status validatePath(const std::vector<DataInputConstRef<DataType>>&,
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
    switch (config.matcher_type_case()) {
    case envoy::config::common::matcher::v3::Matcher::kMatcherTree: {
      auto tree_matcher = createTreeMatcher(config);
      return tree_matcher;
    }
    case envoy::config::common::matcher::v3::Matcher::kMatcherList: {
      auto list_matcher = createListMatcher(config);
      return list_matcher;
    }
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
      return nullptr;
    }
  }

private:
  //
  struct Context {
    Context(MatchTreeFactory<DataType>& factory, const DataInput<DataType>& input)
        : factory_(factory) {
      factory_.current_path_.push_back(std::cref(input));
    }
    ~Context() { factory_.current_path_.pop_back(); }

    MatchTreeFactory<DataType>& factory_;
  };

  template <class T>
  using NodeCreationContext = std::pair<T, std::vector<std::unique_ptr<Context>>>;

  MatchTreeSharedPtr<DataType>
  createListMatcher(const envoy::config::common::matcher::v3::Matcher& config) {
    auto list_matcher =
        std::make_shared<ListMatcher<DataType>>(createOnMatch(config.on_no_match()));

    for (const auto& matcher : config.matcher_list().matchers()) {
      auto field_matcher_context = createFieldMatcher(matcher.predicate());
      list_matcher->addMatcher(std::move(field_matcher_context.first),
                               *createOnMatch(matcher.on_match()));
    }

    return list_matcher;
  }

  NodeCreationContext<FieldMatcherPtr<DataType>> createFieldMatcher(
      const envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate& field_predicate) {
    switch (field_predicate.match_type_case()) {
    case (envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::kSinglePredicate): {
      auto data_input_context = createDataInput(field_predicate.single_predicate().input());
      auto field_matcher = std::make_unique<SingleFieldMatcher<DataType>>(
          std::move(data_input_context.first),
          createInputMatcher(field_predicate.single_predicate()));

      return {std::move(field_matcher), std::move(data_input_context.second)};
    }
    case (envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::kOrMatcher): {
      std::vector<std::unique_ptr<Context>> contexts;
      std::vector<FieldMatcherPtr<DataType>> sub_matchers;
      for (const auto& predicate : field_predicate.or_matcher().predicate()) {
        auto field_matcher_context = createFieldMatcher(predicate);

        for (auto&& c : field_matcher_context.second) {
          contexts.emplace_back(std::move(c));
        }

        sub_matchers.emplace_back(std::move(field_matcher_context.first));
      }

      return {std::make_unique<AnyFieldMatcher<DataType>>(std::move(sub_matchers)),
              std::move(contexts)};
    }
    case (envoy::config::common::matcher::v3::Matcher::MatcherList::Predicate::kAndMatcher): {
      std::vector<std::unique_ptr<Context>> contexts;
      std::vector<FieldMatcherPtr<DataType>> sub_matchers;
      for (const auto& predicate : field_predicate.and_matcher().predicate()) {
        auto field_matcher_context = createFieldMatcher(predicate);

        for (auto&& c : field_matcher_context.second) {
          contexts.emplace_back(std::move(c));
        }

        sub_matchers.emplace_back(std::move(field_matcher_context.first));
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

      auto multimap_matcher = std::make_shared<ExactMapMatcher<DataType>>(
          std::move(data_input_context.first), createOnMatch(matcher.on_no_match()));

      for (const auto& children : matcher.matcher_tree().exact_match_map().map()) {
        multimap_matcher->addChild(children.first,
                                   *MatchTreeFactory::createOnMatch(children.second));
      }

      return multimap_matcher;
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
  createOnMatch(const envoy::config::common::matcher::v3::Matcher::OnMatch& on_match) {
    if (on_match.has_matcher()) {
      return OnMatch<DataType>{{}, create(on_match.matcher())};
    } else if (on_match.has_action()) {
      auto& factory = Config::Utility::getAndCheckFactory<ActionFactory>(on_match.action());
      ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
          on_match.action().typed_config(), factory_context_.messageValidationVisitor(), factory);
      auto action = factory.createActionFactoryCb(*message, "", factory_context_);

      auto status = validator_.validatePath(current_path_, factory.configType());
      if (!status.ok()) {
        throw EnvoyException(fmt::format("match tree validation failed: {}", status));
      }

      return OnMatch<DataType>{factory.createActionFactoryCb(*message, "", factory_context_), {}};
    }

    return absl::nullopt;
  }

  NodeCreationContext<DataInputPtr<DataType>>
  createDataInput(const envoy::config::core::v3::TypedExtensionConfig& config) {
    auto& factory = Config::Utility::getAndCheckFactory<DataInputFactory<DataType>>(config);
    ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
        config.typed_config(), factory_context_.messageValidationVisitor(), factory);
    auto input = factory.createDataInput(*message, factory_context_);

    auto context = std::make_unique<Context>(*this, *input);
    NodeCreationContext<DataInputPtr<DataType>> creation_context;
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
  Server::Configuration::FactoryContext& factory_context_;
  MatchTreeValidator<DataType>& validator_;
};
} // namespace Matcher
} // namespace Envoy
