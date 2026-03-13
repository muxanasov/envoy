#include "envoy/common/exception.h"
#include "envoy/type/matcher/v3/regex.pb.h"

#include "source/common/common/regex.h"

#include "test/test_common/logging.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"
#include <cassert>

using testing::ContainsRegex;

namespace Envoy {
namespace Regex {
namespace {

TEST(Utility, ParseRegex) {
  Regex::GoogleReEngine engine;
  {
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.mutable_google_re2();
    matcher.set_regex("(+invalid)");
    EXPECT_EQ(Utility::parseRegex(matcher, engine).status().message(),
              "no argument for repetition operator: +");
  }

  // Regression test for https://github.com/envoyproxy/envoy/issues/7728
  {
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.mutable_google_re2();
    matcher.set_regex("/asdf/.*");
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    const std::string long_string = "/asdf/" + std::string(50 * 1024, 'a');
    EXPECT_TRUE(compiled_matcher->match(long_string));
  }

  // Regression test for https://github.com/envoyproxy/envoy/issues/15826
  {
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.mutable_google_re2();
    matcher.set_regex("/status/200(/.*)?$");
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    EXPECT_TRUE(compiled_matcher->match("/status/200"));
    EXPECT_TRUE(compiled_matcher->match("/status/200/"));
    EXPECT_TRUE(compiled_matcher->match("/status/200/foo"));
    EXPECT_FALSE(compiled_matcher->match("/status/200foo"));
  }

  // Positive case to ensure no max program size is enforced.
  {
    TestScopedRuntime scoped_runtime;
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
  }

  // Positive case to ensure matcher can be created by config without google_re2 field.
  {
    TestScopedRuntime scoped_runtime;
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
  }

  // Verify max program size with the deprecated field codepath plus runtime.
  // The deprecated field codepath precedes any runtime settings.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"re2.max_program_size.error_level", "3"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2()->mutable_max_program_size()->set_value(1);
#ifndef GTEST_USES_SIMPLE_RE
    EXPECT_THAT(Utility::parseRegex(matcher, engine).status().message(),
                ContainsRegex("RE2 program size of [0-9]+ > max program size of 1\\."));
#else
    EXPECT_THAT(Utility::parseRegex(matcher, engine).status().message(),
                ContainsRegex("RE2 program size of \\d+ > max program size of 1\\."));
#endif
  }

  // Verify that an exception is thrown for the error level max program size.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"re2.max_program_size.error_level", "1"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
#ifndef GTEST_USES_SIMPLE_RE
    EXPECT_THAT(Utility::parseRegex(matcher, engine).status().message(),
                ContainsRegex("RE2 program size of [0-9]+ > max program size of 1 set for the "
                              "error level threshold\\."));
#else
    EXPECT_THAT(Utility::parseRegex(matcher, engine).status().message(),
                ContainsRegex("RE2 program size of \\d+ > max program size of 1 set for the error "
                              "level threshold\\."))
#endif
  }

  // Verify that the error level max program size defaults to 100 if not set by runtime.
  {
    TestScopedRuntime scoped_runtime;
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex(
        "/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*");
    matcher.mutable_google_re2();
#ifndef GTEST_USES_SIMPLE_RE
    EXPECT_THAT(Utility::parseRegex(matcher, engine).status().message(),
                ContainsRegex("RE2 program size of [0-9]+ > max program size of 100 set for the "
                              "error level threshold\\."));
#else
    EXPECT_THAT(Utility::parseRegex(matcher, engine).status().message(),
                ContainsRegex("RE2 program size of \\d+ > max program size of 100 set for the "
                              "error level threshold\\."));
#endif
  }

  // Verify that a warning is logged for the warn level max program size.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"re2.max_program_size.warn_level", "1"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    EXPECT_LOG_CONTAINS("warn", "> max program size of 1 set for the warn level threshold",
                        *Utility::parseRegex(matcher, engine));
  }

  // Verify that no check is performed if the warn level max program size is not set by runtime.
  {
    TestScopedRuntime scoped_runtime;
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    EXPECT_LOG_NOT_CONTAINS("warn", "> max program size", *Utility::parseRegex(matcher, engine));
  }
}

TEST(GoogleReEngineTest, Matcher) {
  GoogleReEngine engine;

  // Test lazy path (LazyGoogleReMatcher)
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.mutable_google_re2();
    matcher.set_regex("(+invalid)");
    // with lazy loading enabled, the regex is not compiled at creation time, 
    // so no error should be thrown until the regex is actually accessed.
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    // rexex is compile at the first access; any operation will terun false in case of error
    EXPECT_FALSE(compiled_matcher->match("test"));
    // the error is stored in logs.
    EXPECT_LOG_CONTAINS("error", "Invalid regex: no argument for repetition operator: +", compiled_matcher->match("test"));
  }

  // Regression test for https://github.com/envoyproxy/envoy/issues/7728
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.mutable_google_re2();
    matcher.set_regex("/asdf/.*");
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    const std::string long_string = "/asdf/" + std::string(50 * 1024, 'a');
    EXPECT_TRUE(compiled_matcher->match(long_string));
  }

  // Regression test for https://github.com/envoyproxy/envoy/issues/15826
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.mutable_google_re2();
    matcher.set_regex("/status/200(/.*)?$");
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    EXPECT_TRUE(compiled_matcher->match("/status/200"));
    EXPECT_TRUE(compiled_matcher->match("/status/200/"));
    EXPECT_TRUE(compiled_matcher->match("/status/200/foo"));
    EXPECT_FALSE(compiled_matcher->match("/status/200foo"));
  }

  // Positive case to ensure no max program size is enforced.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
  }

  // Positive case to ensure matcher can be created by config without google_re2 field.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
  }

  // Verify max program size with the deprecated field codepath plus runtime.
  // The deprecated field codepath precedes any runtime settings.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    scoped_runtime.mergeValues({{"re2.max_program_size.error_level", "3"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2()->mutable_max_program_size()->set_value(1);
    // with lazy loading enabled, the regex is not validated at creation time, so no error should be thrown until the regex is actually accessed.
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    // after accessing, the error is stored in logs
    EXPECT_FALSE(compiled_matcher->match("/asdf/a"));
    EXPECT_LOG_CONTAINS("error", "Increase configured max program size if necessary.", compiled_matcher->match("test"));
  }

  // Verify that an exception is thrown for the error level max program size.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    scoped_runtime.mergeValues({{"re2.max_program_size.error_level", "1"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    // with lazy loading enabled, the regex is not validated at creation time, so no error should be thrown until the regex is actually accessed.
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    // after accessing, the error is stored in logs
    EXPECT_FALSE(compiled_matcher->match("/asdf/a"));
    EXPECT_LOG_CONTAINS("error", "Increase configured max program size if necessary.", compiled_matcher->match("test"));
  }

  // Verify that the error level max program size defaults to 100 if not set by runtime.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex(
        "/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*/asdf/.*");
    matcher.mutable_google_re2();
    // with lazy loading enabled, the regex is not validated at creation time, so no error should be thrown until the regex is actually accessed.
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    // after accessing, the error is stored in logs
    EXPECT_FALSE(compiled_matcher->match("/asdf/a/asdf/a/asdf/a/asdf/a/asdf/a/asdf/a/asdf/a/asdf/a/asdf/a/asdf/a"));
    EXPECT_LOG_CONTAINS("error", "Increase configured max program size if necessary.", compiled_matcher->match("test"));
  }

  // Verify that a warning is logged for the warn level max program size.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    scoped_runtime.mergeValues({{"re2.max_program_size.warn_level", "1"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    // with lazy loading enabled, the regex is not validated at creation time, so no error should be thrown until the regex is actually accessed.
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    EXPECT_LOG_CONTAINS("warn", "> max program size of 1 set for the warn level threshold", compiled_matcher->match("/asdf/a"));
    EXPECT_TRUE(compiled_matcher->match("/asdf/a"));
  }

  // Verify that no check is performed if the warn level max program size is not set by runtime.
  {
    TestScopedRuntime scoped_runtime;
    scoped_runtime.mergeValues({{"envoy.reloadable_features.re2_lazy_loading", "true"}});
    envoy::type::matcher::v3::RegexMatcher matcher;
    matcher.set_regex("/asdf/.*");
    matcher.mutable_google_re2();
    EXPECT_TRUE(Utility::parseRegex(matcher, engine).status().ok());
    const auto compiled_matcher = *Utility::parseRegex(matcher, engine);
    EXPECT_TRUE(compiled_matcher->match("/asdf/a"));
    EXPECT_LOG_NOT_CONTAINS("warn", "> max program size", *Utility::parseRegex(matcher, engine));
  }
}

} // namespace
} // namespace Regex
} // namespace Envoy
