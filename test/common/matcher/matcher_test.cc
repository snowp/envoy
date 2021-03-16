#include <bits/stdint-uintn.h>

#include <exception>
#include <memory>
#include <vector>

#include "envoy/config/common/matcher/v3/matcher.pb.validate.h"
#include "envoy/config/core/v3/extension.pb.h"
#include "envoy/matcher/matcher.h"
#include "envoy/registry/registry.h"

#include "common/matcher/list_matcher.h"
#include "common/matcher/matcher.h"
#include "common/protobuf/utility.h"

#include "test/common/matcher/test_utility.h"
#include "test/mocks/server/factory_context.h"
#include "test/test_common/registry.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

using testing::ContainerEq;
using testing::SizeIs;
using testing::UnorderedElementsAreArray;

namespace Envoy {
namespace Matcher {

struct TrackingTreeValidator : public MatchTreeValidator<TestData> {

  absl::Status validatePath(const std::vector<DataInputConstRef<TestData>>& path,
                            const std::string& action_type_url) override {
    PathWithAction path_with_action;
    path_with_action.action_type_url_ = action_type_url;

    for (const auto& p : path) {
      path_with_action.input_values_.push_back(
          std::string(static_cast<const TestInput&>(p.get()).result_.data_.value_or("")));
    }

    paths_.push_back(std::move(path_with_action));

    return absl::OkStatus();
  }

  struct PathWithAction {
    std::vector<std::string> input_values_;
    std::string action_type_url_;

    bool operator==(const PathWithAction& other) const {
      return input_values_ == other.input_values_ && action_type_url_ == other.action_type_url_;
    }
  };

  void expectPaths(const std::vector<PathWithAction>& paths) {
    EXPECT_THAT(paths, UnorderedElementsAreArray(paths));
  }

  std::vector<PathWithAction> paths_;
};

class MatcherTest : public ::testing::Test {
public:
  MatcherTest() : inject_action_(action_factory_), factory_(factory_context_, validator_) {}

  StringActionFactory action_factory_;
  Registry::InjectFactory<ActionFactory> inject_action_;
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  TrackingTreeValidator validator_;
  MatchTreeFactory<TestData> factory_;
};

TEST_F(MatcherTest, TestMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: outer_input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  exact_match_map:
    map:
      value:
        matcher:
          matcher_list:
            matchers:
            - on_match:
                action:
                  name: test_action
                  typed_config:
                    "@type": type.googleapis.com/google.protobuf.StringValue
                    value: match!!
              predicate:
                single_predicate:
                  input:
                    name: inner_input
                    typed_config:
                      "@type": type.googleapis.com/google.protobuf.StringValue
                  value_match:
                    exact: foo
  )EOF";

  envoy::config::common::matcher::v3::Matcher matcher;
  MessageUtil::loadFromYaml(yaml, matcher, ProtobufMessage::getStrictValidationVisitor());

  TestUtility::validate(matcher);

  auto outer_factory = TestDataInputFactory("outer_input", "value");
  auto inner_factory = TestDataInputFactory("inner_input", "foo");

  auto match_tree = factory_.create(matcher);

  const auto result = match_tree->match(TestData());
  EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
  EXPECT_TRUE(result.on_match_.has_value());
  EXPECT_NE(result.on_match_->action_cb_, nullptr);
}

TEST_F(MatcherTest, CustomMatcher) {
  const std::string yaml = R"EOF(
matcher_list:
  matchers:
  - on_match:
      action:
        name: test_action
        typed_config:
          "@type": type.googleapis.com/google.protobuf.StringValue
          value: match!!
    predicate:
      single_predicate:
        input:
          name: inner_input
          typed_config:
            "@type": type.googleapis.com/google.protobuf.StringValue
        custom_match:
          name: never_match
          typed_config: {}
  )EOF";

  envoy::config::common::matcher::v3::Matcher matcher;
  MessageUtil::loadFromYaml(yaml, matcher, ProtobufMessage::getStrictValidationVisitor());

  TestUtility::validate(matcher);

  auto inner_factory = TestDataInputFactory("inner_input", "foo");
  NeverMatchFactory match_factory;

  auto match_tree = factory_.create(matcher);

  const auto result = match_tree->match(TestData());
  EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
  EXPECT_FALSE(result.on_match_.has_value());
}

TEST_F(MatcherTest, TestAndMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: outer_input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  exact_match_map:
    map:
      value:
        matcher:
          matcher_list:
            matchers:
            - on_match:
                action:
                  name: test_action
                  typed_config:
                    "@type": type.googleapis.com/google.protobuf.StringValue
                    value: match!!
              predicate:
                and_matcher:
                  predicate:
                  - single_predicate:
                      input:
                        name: inner_input
                        typed_config:
                          "@type": type.googleapis.com/google.protobuf.StringValue
                      value_match:
                        exact: foo
                  - single_predicate:
                      input:
                        name: inner_input
                        typed_config:
                          "@type": type.googleapis.com/google.protobuf.StringValue
                      value_match:
                        exact: foo
  )EOF";

  envoy::config::common::matcher::v3::Matcher matcher;
  MessageUtil::loadFromYaml(yaml, matcher, ProtobufMessage::getStrictValidationVisitor());

  TestUtility::validate(matcher);

  auto outer_factory = TestDataInputFactory("outer_input", "value");
  auto inner_factory = TestDataInputFactory("inner_input", "foo");

  auto match_tree = factory_.create(matcher);

  validator_.expectPaths({{{"value", "foo", "foo"}, "google.protobuf.StringValue"}});

  const auto result = match_tree->match(TestData());
  EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
  EXPECT_TRUE(result.on_match_.has_value());
  EXPECT_NE(result.on_match_->action_cb_, nullptr);
}

TEST_F(MatcherTest, TestMultipleBranches) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: outer_input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  exact_match_map:
    map:
      outer:
        matcher:
          matcher_list:
            matchers:
            - on_match:
                action:
                  name: test_action
                  typed_config:
                    "@type": type.googleapis.com/google.protobuf.StringValue
                    value: match!!
              predicate:
                single_predicate:
                  input:
                    name: first_inner
                    typed_config:
                      "@type": type.googleapis.com/google.protobuf.StringValue
                  value_match:
                    exact: first
      "not-used":
        matcher:
          matcher_list:
            matchers:
            - on_match:
                action:
                  name: test_action
                  typed_config:
                    "@type": type.googleapis.com/google.protobuf.StringValue
                    value: match!!
              predicate:
                single_predicate:
                  input:
                    name: second_inner
                    typed_config:
                      "@type": type.googleapis.com/google.protobuf.StringValue
                  value_match:
                    exact: second
  )EOF";

  envoy::config::common::matcher::v3::Matcher matcher;
  MessageUtil::loadFromYaml(yaml, matcher, ProtobufMessage::getStrictValidationVisitor());

  TestUtility::validate(matcher);

  auto outer_factory = TestDataInputFactory("outer_input", "outer");
  auto first_inner_factory = TestDataInputFactory("first_inner", "first");
  auto second_inner_factory = TestDataInputFactory("second_inner", "second");

  auto match_tree = factory_.create(matcher);

  EXPECT_EQ(2, validator_.paths_.size());
  validator_.expectPaths({{{"outer", "second"}, "google.protobuf.StringValue"},
                          {{"outer", "first"}, "google.protobuf.StringValue"}});

  const auto result = match_tree->match(TestData());
  EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
  EXPECT_TRUE(result.on_match_.has_value());
  EXPECT_NE(result.on_match_->action_cb_, nullptr);
} // namespace Matcher

TEST_F(MatcherTest, TestOrMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: outer_input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  exact_match_map:
    map:
      value:
        matcher:
          matcher_list:
            matchers:
            - on_match:
                action:
                  name: test_action
                  typed_config:
                    "@type": type.googleapis.com/google.protobuf.StringValue
                    value: match!!
              predicate:
                or_matcher:
                  predicate:
                  - single_predicate:
                      input:
                        name: inner_input
                        typed_config:
                          "@type": type.googleapis.com/google.protobuf.StringValue
                      value_match:
                        exact: foo
                  - single_predicate:
                      input:
                        name: inner_input
                        typed_config:
                          "@type": type.googleapis.com/google.protobuf.StringValue
                      value_match:
                        exact: foo
  )EOF";

  envoy::config::common::matcher::v3::Matcher matcher;
  MessageUtil::loadFromYaml(yaml, matcher, ProtobufMessage::getStrictValidationVisitor());

  TestUtility::validate(matcher);

  auto outer_factory = TestDataInputFactory("outer_input", "value");
  auto inner_factory = TestDataInputFactory("inner_input", "foo");

  auto match_tree = factory_.create(matcher);

  EXPECT_EQ(1, validator_.paths_.size());
  validator_.expectPaths({{{"value", "foo", "foo"}, "google.protobuf.StringValue"}});

  const auto result = match_tree->match(TestData());
  EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
  EXPECT_TRUE(result.on_match_.has_value());
  EXPECT_NE(result.on_match_->action_cb_, nullptr);
}

TEST_F(MatcherTest, TestRecursiveMatcher) {
  const std::string yaml = R"EOF(
matcher_list:
  matchers:
  - on_match:
      matcher:
        matcher_list:
          matchers:
          - on_match:
              action:
                name: test_action
                typed_config:
                  "@type": type.googleapis.com/google.protobuf.StringValue
                  value: match!!
            predicate:
              single_predicate:
                input:
                  name: inner_input
                  typed_config:
                    "@type": type.googleapis.com/google.protobuf.StringValue
                value_match:
                  exact: foo
    predicate:
      single_predicate:
        input:
          name: outer_input
          typed_config:
            "@type": type.googleapis.com/google.protobuf.StringValue
        value_match:
          exact: value
  )EOF";

  envoy::config::common::matcher::v3::Matcher matcher;
  MessageUtil::loadFromYaml(yaml, matcher, ProtobufMessage::getStrictValidationVisitor());

  TestUtility::validate(matcher);

  auto outer_factory = TestDataInputFactory("outer_input", "value");
  auto inner_factory = TestDataInputFactory("inner_input", "foo");

  auto match_tree = factory_.create(matcher);

  EXPECT_EQ(1, validator_.paths_.size());
  validator_.expectPaths({{{"value", "foo"}, "google.protobuf.StringValue"}});

  const auto result = match_tree->match(TestData());
  EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
  EXPECT_TRUE(result.on_match_.has_value());
  EXPECT_EQ(result.on_match_->action_cb_, nullptr);

  const auto recursive_result = evaluateMatch(*match_tree, TestData());
  EXPECT_EQ(recursive_result.match_state_, MatchState::MatchComplete);
  EXPECT_NE(recursive_result.result_, nullptr);
}

TEST_F(MatcherTest, RecursiveMatcherNoMatch) {
  ListMatcher<TestData> matcher(absl::nullopt);

  matcher.addMatcher(createSingleMatcher(absl::nullopt, [](auto) { return false; }),
                     stringOnMatch<TestData>("match"));

  const auto recursive_result = evaluateMatch(matcher, TestData());
  EXPECT_EQ(recursive_result.match_state_, MatchState::MatchComplete);
  EXPECT_EQ(recursive_result.result_, nullptr);
}

TEST_F(MatcherTest, RecursiveMatcherCannotMatch) {
  ListMatcher<TestData> matcher(absl::nullopt);

  matcher.addMatcher(createSingleMatcher(
                         absl::nullopt, [](auto) { return false; },
                         DataInputGetResult::DataAvailability::NotAvailable),
                     stringOnMatch<TestData>("match"));

  const auto recursive_result = evaluateMatch(matcher, TestData());
  EXPECT_EQ(recursive_result.match_state_, MatchState::UnableToMatch);
  EXPECT_EQ(recursive_result.result_, nullptr);
}
} // namespace Matcher
} // namespace Envoy
