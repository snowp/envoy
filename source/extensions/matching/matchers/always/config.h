#pragma once

#include <memory>

#include "common/matcher/matcher.h"

#include "external/envoy_api/envoy/config/core/v3/extension.pb.h"

namespace Envoy {

class AlwaysTrueMatcher : public Envoy::InputMatcher {
public:
  absl::optional<bool> match(absl::string_view) override { return true; }
};

class AlwaysTrueMatcherFactory : public InputMatcherFactory {
public:
  InputMatcherPtr create(const envoy::config::core::v3::TypedExtensionConfig&) override {
    return std::make_unique<AlwaysTrueMatcher>();
  }

  std::string name() const override { return "envoy.matcher.matchers.always"; }
  std::string category() const override { return "envoy.matcher.matchers"; }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<ProtobufWkt::Empty>();
  }
};

} // namespace Envoy