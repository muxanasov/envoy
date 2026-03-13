#include "source/common/common/regex.h"

#include "envoy/common/exception.h"
#include "envoy/extensions/regex_engines/v3/google_re2.pb.h"
#include "envoy/extensions/regex_engines/v3/google_re2.pb.validate.h"
#include "envoy/type/matcher/v3/regex.pb.h"
#include "envoy/type/matcher/v3/regex.pb.validate.h"

#include "re2/re2.h"
#include "regex.h"
#include "source/common/common/assert.h"
#include "source/common/common/fmt.h"
#include "source/common/runtime/runtime_features.h"

namespace Envoy {
namespace Regex {

absl::StatusOr<std::unique_ptr<CompiledGoogleReMatcher>>
CompiledGoogleReMatcher::createAndSizeCheck(const std::string& regex) {
  absl::Status creation_status = absl::OkStatus();
  auto ret = std::unique_ptr<CompiledGoogleReMatcher>(
      new CompiledGoogleReMatcher(regex, creation_status, true));
  RETURN_IF_NOT_OK(creation_status);
  return ret;
}

absl::StatusOr<std::unique_ptr<CompiledGoogleReMatcher>>
CompiledGoogleReMatcher::create(const envoy::type::matcher::v3::RegexMatcher& config) {
  absl::Status creation_status = absl::OkStatus();
  auto ret = std::unique_ptr<CompiledGoogleReMatcher>(
      new CompiledGoogleReMatcher(config, creation_status));
  RETURN_IF_NOT_OK(creation_status);
  return ret;
}

absl::StatusOr<std::unique_ptr<CompiledGoogleReMatcher>>
CompiledGoogleReMatcher::create(const xds::type::matcher::v3::RegexMatcher& config) {
  absl::Status creation_status = absl::OkStatus();
  auto ret = std::unique_ptr<CompiledGoogleReMatcher>(
      new CompiledGoogleReMatcher(config, creation_status));
  RETURN_IF_NOT_OK(creation_status);
  return ret;
}

absl::StatusOr<std::unique_ptr<LazyGoogleReMatcher>>
LazyGoogleReMatcher::createAndSizeCheck(const std::string& regex) {
  auto ret = std::unique_ptr<LazyGoogleReMatcher>(
      new LazyGoogleReMatcher(regex, true));
  return ret;
}

absl::StatusOr<std::unique_ptr<LazyGoogleReMatcher>>
LazyGoogleReMatcher::create(const envoy::type::matcher::v3::RegexMatcher& config) {
  std::optional<uint32_t> max_program_size = std::nullopt;
  // Check if the deprecated field max_program_size is set first, and follow the old logic if so.
  if (config.google_re2().has_max_program_size()) {
    max_program_size = std::optional<uint32_t>(
        PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.google_re2(), max_program_size, 100));
  }
  auto ret = std::unique_ptr<LazyGoogleReMatcher>(
      new LazyGoogleReMatcher(config, max_program_size));
  return ret;
}

absl::StatusOr<std::unique_ptr<LazyGoogleReMatcher>>
LazyGoogleReMatcher::create(const xds::type::matcher::v3::RegexMatcher& config) {
  auto ret = std::unique_ptr<LazyGoogleReMatcher>(
      new LazyGoogleReMatcher(config));
  return ret;
}

absl::StatusOr<re2::RE2*> SafeLazyRE2::get() const {
  absl::call_once(once_, &SafeLazyRE2::Init, this);
  ASSERT(regex_ != nullptr);
  RETURN_IF_NOT_OK(creation_status_);
  return regex_.get();
}

void SafeLazyRE2::Init(const SafeLazyRE2* safe_lazy_re2) {
  safe_lazy_re2->creation_status_ = absl::OkStatus();
  auto regex = std::make_unique<re2::RE2>(safe_lazy_re2->pattern_, safe_lazy_re2->options_);
  safe_lazy_re2->regex_ = std::move(regex);
  if (!safe_lazy_re2->regex_->ok()) {
    safe_lazy_re2->creation_status_ = absl::InvalidArgumentError(safe_lazy_re2->regex_->error());
    return;
  }
  const uint32_t regex_program_size = static_cast<uint32_t>(safe_lazy_re2->regex_->ProgramSize());
  // Check if the deprecated field max_program_size is set first, and follow the old logic if so.
  if (safe_lazy_re2->deprecated_max_program_size_.has_value()) {
    const uint32_t max_program_size = safe_lazy_re2->deprecated_max_program_size_.value();
    if (regex_program_size > max_program_size) {
      safe_lazy_re2->creation_status_ = absl::InvalidArgumentError(
          fmt::format("regex '{}' RE2 program size of {} > max program size of "
                      "{}. Increase configured max program size if necessary.",
                      safe_lazy_re2->pattern_, regex_program_size, max_program_size));
      return;
    }
  }

  if (safe_lazy_re2->do_program_size_check_ && Runtime::isRuntimeInitialized()) {
    const uint32_t max_program_size_error_level =
        Runtime::getInteger("re2.max_program_size.error_level", 100);

    if (regex_program_size > max_program_size_error_level) {
      safe_lazy_re2->creation_status_ = absl::InvalidArgumentError(
          fmt::format("regex '{}' RE2 program size of {} > max program size of "
                      "{} set for the error level threshold. Increase "
                      "configured max program size if necessary.",
                      safe_lazy_re2->pattern_, regex_program_size, max_program_size_error_level));
      return;
    }

    const uint32_t max_program_size_warn_level =
        Runtime::getInteger("re2.max_program_size.warn_level", UINT32_MAX);

    if (regex_program_size > max_program_size_warn_level) {
      ENVOY_LOG_MISC(warn,
                     "regex '{}' RE2 program size of {} > max program size of {} set for the warn "
                     "level threshold. Increase configured max program size if necessary.",
                     safe_lazy_re2->pattern_, regex_program_size, max_program_size_warn_level);
    }
  }
}

bool LazyGoogleReMatcher::match(absl::string_view value) const { 
  auto regex_or_status = regex_.get();
  if (!regex_or_status.ok()) {
    ENVOY_LOG_MISC(error, "Invalid regex: {}", regex_or_status.status().message());
    return false;
  }
  ASSERT(regex_or_status.value() != nullptr);
  return re2::RE2::FullMatch(value, *regex_or_status.value()); 
}


std::string LazyGoogleReMatcher::replaceAll(absl::string_view value, absl::string_view substitution) const {
  std::string result = std::string(value);
  auto regex_or_status = regex_.get();
  if (!regex_or_status.ok()) {
    ENVOY_LOG_MISC(error, "Invalid regex: {}", regex_or_status.status().message());
    return result;
  }
  re2::RE2::GlobalReplace(&result, *regex_or_status.value(), substitution);
  return result;
}

const std::string& LazyGoogleReMatcher::pattern() const { 
  auto regex_or_status = regex_.get();
  if (!regex_or_status.ok()) {
    ENVOY_LOG_MISC(error, "Invalid regex: {}", regex_or_status.status().message());
    return pattern_;
  }
  return regex_or_status.value()->pattern();
}

REGISTER_FACTORY(GoogleReEngineFactory, EngineFactory);
CompiledGoogleReMatcher::CompiledGoogleReMatcher(const std::string& regex,
                                                 absl::Status& creation_status,
                                                 bool do_program_size_check)
    : regex_(regex, re2::RE2::Quiet) {
  if (!regex_.ok()) {
    creation_status = absl::InvalidArgumentError(regex_.error());
    return;
  }

  if (do_program_size_check && Runtime::isRuntimeInitialized()) {
    const uint32_t regex_program_size = static_cast<uint32_t>(regex_.ProgramSize());
    const uint32_t max_program_size_error_level =
        Runtime::getInteger("re2.max_program_size.error_level", 100);
    if (regex_program_size > max_program_size_error_level) {
      creation_status = absl::InvalidArgumentError(
          fmt::format("regex '{}' RE2 program size of {} > max program size of "
                      "{} set for the error level threshold. Increase "
                      "configured max program size if necessary.",
                      regex, regex_program_size, max_program_size_error_level));
    }

    const uint32_t max_program_size_warn_level =
        Runtime::getInteger("re2.max_program_size.warn_level", UINT32_MAX);
    if (regex_program_size > max_program_size_warn_level) {
      ENVOY_LOG_MISC(warn,
                     "regex '{}' RE2 program size of {} > max program size of {} set for the warn "
                     "level threshold. Increase configured max program size if necessary.",
                     regex, regex_program_size, max_program_size_warn_level);
    }
  }
}

CompiledGoogleReMatcher::CompiledGoogleReMatcher(
    const envoy::type::matcher::v3::RegexMatcher& config, absl::Status& creation_status)
    : CompiledGoogleReMatcher(config.regex(), creation_status,
                              !config.google_re2().has_max_program_size()) {
  const uint32_t regex_program_size = static_cast<uint32_t>(regex_.ProgramSize());

  // Check if the deprecated field max_program_size is set first, and follow the old logic if so.
  if (config.google_re2().has_max_program_size()) {
    const uint32_t max_program_size =
        PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.google_re2(), max_program_size, 100);
    if (regex_program_size > max_program_size) {
      creation_status = absl::InvalidArgumentError(
          fmt::format("regex '{}' RE2 program size of {} > max program size of "
                      "{}. Increase configured max program size if necessary.",
                      config.regex(), regex_program_size, max_program_size));
    }
  }
}

absl::StatusOr<CompiledMatcherPtr> GoogleReEngine::matcher(const std::string& regex) const {
  if(Runtime::runtimeFeatureEnabled("envoy.reloadable_features.re2_lazy_loading")) {
    return LazyGoogleReMatcher::createAndSizeCheck(regex);
  }
  return CompiledGoogleReMatcher::createAndSizeCheck(regex);
}

EnginePtr GoogleReEngineFactory::createEngine(const Protobuf::Message&,
                                              Server::Configuration::ServerFactoryContext&) {
  return std::make_shared<GoogleReEngine>();
}

ProtobufTypes::MessagePtr GoogleReEngineFactory::createEmptyConfigProto() {
  return std::make_unique<envoy::extensions::regex_engines::v3::GoogleRE2>();
}

} // namespace Regex
} // namespace Envoy
