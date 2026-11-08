#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include <retry/retry.hpp>

using namespace crn::retry;
using namespace std::chrono_literals;

using opt_string = std::optional<std::string>;
using ms = crn::retry::duration_t;

TEST(retry, nothing_runs_until_the_description_is_invoked) {
    int calls = 0;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{1}},
        .delay = 0ms,
    };

    auto retrying = crn::make_retry(policy,
                                    [&calls]() -> opt_string {
                                        calls++;
                                        return "This is valid";
                                    },
                                    {
                                        .delay = [](ms) {},
                                    });

    ASSERT_EQ(calls, 0);

    auto result = retrying();

    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(result);
}

TEST(retry, a_description_can_be_invoked_twice) {
    int calls = 0;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{1}},
        .delay = 0ms,
    };

    auto retrying = crn::make_retry(policy,
                                    [&calls]() -> opt_string {
                                        calls++;
                                        return "This is valid";
                                    },
                                    {
                                        .delay = [](ms) {},
                                    });

    auto first = retrying();
    auto second = retrying();

    EXPECT_EQ(calls, 2);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
}

TEST(retry, fail) {
    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{1}},
        .delay = 0ms,
    };

    auto retrying = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                    {
                                        .delay = [](ms) {},
                                    });

    auto result = retrying();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().attempts, 1);
    EXPECT_EQ(result.error().waited, 0ms);
}

TEST(retry, fail_bool) {
    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{1}},
        .delay = 0ms,
    };

    auto retrying = crn::make_retry(policy, []() { return false; },
                                    {
                                        .delay = [](ms) {},
                                    });

    auto result = retrying();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().attempts, 1);
}

TEST(retry, fail_with_custom_check) {
    int count = 0;
    const int attempts = 5;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{attempts}},
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy, []() -> opt_string { return "This is valid"; },
                                  {
                                      .check =
                                          [&count](const opt_string& /*unused*/) {
                                              ++count;
                                              return false;
                                          },
                                      .delay = [](ms) {},
                                  })();

    EXPECT_FALSE(result);
    EXPECT_EQ(count, attempts);
}

TEST(retry, a_rejected_result_is_not_an_exception) {
    // An attempt whose result the check rejects is an ordinary failure: there
    // is no cause to report, only the fact that it never passed.
    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{1}},
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                  {
                                      .delay = [](ms) {},
                                  })();

    ASSERT_FALSE(result);
    EXPECT_FALSE(result.error().cause);
}

TEST(retry, a_throwing_action_reports_the_last_exception) {
    // The old implementation swallowed every exception, so a retry that failed
    // because the action kept throwing was indistinguishable from one that
    // failed because the results were rejected.
    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{3}},
        .delay = 0ms,
    };

    auto result = crn::make_retry(
        policy, []() -> opt_string { throw std::runtime_error("the server said no"); },
        {
            .delay = [](ms) {},
        })();

    ASSERT_FALSE(result);
    ASSERT_TRUE(result.error().cause);
    EXPECT_EQ(result.error().attempts, 3);

    try {
        std::rethrow_exception(result.error().cause);
        FAIL() << "expected the stored exception to be rethrown";
    } catch(const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "the server said no");
    }
}

TEST(retry, the_error_reports_how_long_was_spent_waiting) {
    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{4}},
        .delay = 250ms,
    };

    auto result = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                  {
                                      .delay = [](ms) {},
                                  })();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().attempts, 4);
    EXPECT_EQ(result.error().waited, 3 * 250ms);  // four attempts, three waits
}

TEST(retry, fail_constant_policy_with_transformer_limit_attempts) {
    int attempts = 0;
    const int limit = 5;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{limit}},
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy,
                                  [&attempts]() -> opt_string {
                                      attempts++;
                                      return std::nullopt;
                                  },
                                  {
                                      .delay = [](ms) {},
                                  })();

    ASSERT_FALSE(result);
    EXPECT_EQ(attempts, limit);
    EXPECT_EQ(result.error().attempts, limit);
}

TEST(retry, ringed_exponential_with_jitter_policy) {
    policy::ringed_exponential_with_jitter policy{
        .transformers = {policy::transformer::limit_attempts{10}},
        .base = 500ms,
        .seeder = []() -> std::seed_seq { return {0}; },
        .restart_after = 3,
    };

    size_t attempt = 0;

    auto max_sleep =
        std::vector<ms>{449ms, 961ms, 1985ms, 147ms, 449ms, 961ms, 1985ms, 147ms, 449ms};

    auto result = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                  {
                                      .delay =
                                          [&](ms delay) {
                                              EXPECT_EQ(delay, max_sleep[attempt]);
                                              attempt++;
                                          },
                                  })();

    ASSERT_FALSE(result);
    EXPECT_EQ(attempt, 9);
}

TEST(retry, fail_exponential_policy_with_transformer_limit_by_delay) {
    size_t attempts = 0;
    const size_t expected_attempts = 4;
    std::vector<ms> expected{{200ms, 400ms, 800ms, 1600ms}};

    policy::exponential policy{
        .transformers = {policy::transformer::limit_by_delay{1000ms}},
        .base = 200ms,
    };

    auto result = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                  {
                                      .delay =
                                          [&expected, &attempts](ms delay) {
                                              ASSERT_EQ(expected[attempts], delay);
                                              attempts++;
                                          },
                                  })();

    ASSERT_FALSE(result);
    EXPECT_EQ(attempts, expected_attempts);
}

TEST(retry, fail_exponential_policy_with_transformer_limit_by_cumulative_delay) {
    ms cumulative_delay{0};

    policy::exponential policy{
        .transformers = {policy::transformer::limit_by_cumulative_delay{1000ms}},
        .base = 200ms,
    };

    auto result =
        crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                        {
                            .delay = [&cumulative_delay](ms delay) { cumulative_delay += delay; },
                        })();

    ASSERT_FALSE(result);
    EXPECT_EQ(cumulative_delay, 3000ms);
}

TEST(retry, fail_exponential_policy_with_transformer_cap_delay) {
    const ms cap{1000};
    int cap_reached = 0;
    int cap_still_not_reached = 0;

    std::vector<policy::transformer_t> transformers{
        policy::transformer::limit_attempts{8},
        policy::transformer::cap_delay{cap},
    };

    policy::exponential policy{
        .transformers = transformers,
        .base = 200ms,
    };

    auto result = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                  {
                                      .delay =
                                          [cap, &cap_reached, &cap_still_not_reached](ms delay) {
                                              if(delay == cap)
                                                  cap_reached++;
                                              else
                                                  cap_still_not_reached++;
                                          },
                                  })();

    ASSERT_FALSE(result);
    EXPECT_EQ(cap_reached, 3);
    EXPECT_EQ(cap_still_not_reached, 4);
}

TEST(retry, fail_full_jitter_policy) {
    ms cumulative_delay{0};

    policy::full_jitter policy{
        .transformers = {policy::transformer::limit_attempts{5}},
        .base = 100ms,
        .seeder = []() -> std::seed_seq { return {0}; },
    };

    auto result =
        crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                        {
                            .delay = [&cumulative_delay](ms delay) { cumulative_delay += delay; },
                        })();

    ASSERT_FALSE(result);
    EXPECT_EQ(cumulative_delay, 552ms);
}

TEST(retry, ok_after_fail) {
    int attempts = 0;
    const std::string expected{"Success"};

    policy::constant policy{
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy,
                                  [&attempts, expected]() -> opt_string {
                                      if(attempts == 1) return expected;

                                      attempts++;
                                      return std::nullopt;
                                  },
                                  {
                                      .delay = [](ms) {},
                                  })();

    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value().value(), expected);
    EXPECT_EQ(attempts, 1);
}

TEST(retry, ok_after_fail_bool) {
    int attempts = 0;

    policy::constant policy{
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy,
                                  [&attempts]() -> bool {
                                      if(attempts == 1) return true;

                                      attempts++;
                                      return false;
                                  },
                                  {
                                      .delay = [](ms) {},
                                  })();

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value());
    EXPECT_EQ(attempts, 1);
}

TEST(retry, ok) {
    int attempts = 0;
    const std::string expected{"Success"};

    policy::constant policy{
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy, [expected]() -> opt_string { return expected; },
                                  {
                                      .delay = [&attempts](ms) { attempts++; },
                                  })();

    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value().value(), expected);
    EXPECT_EQ(attempts, 0);
}

TEST(retry, ok_bool) {
    int attempts = 0;

    policy::constant policy{
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy, []() -> bool { return true; },
                                  {
                                      .delay = [&attempts](ms) { attempts++; },
                                  })();

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value());
    EXPECT_EQ(attempts, 0);
}

TEST(retry, an_int_result_is_never_treated_as_a_failure) {
    // Only types with an explicit operator bool carry their own notion of
    // failure. Zero is a perfectly good int.
    int attempts = 0;

    auto result = crn::make_retry(policy::constant{.delay = 0ms},
                                  [&attempts]() -> int {
                                      attempts++;
                                      return 0;
                                  },
                                  {
                                      .delay = [](ms) {},
                                  })();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0);
    EXPECT_EQ(attempts, 1);
}

TEST(retry, the_result_composes_with_the_monadic_operations) {
    auto result =
        crn::make_retry(policy::constant{.delay = 0ms}, []() -> opt_string { return "7"; },
                        {
                            .delay = [](ms) {},
                        })()
            .transform([](const opt_string& value) { return std::stoi(value.value()); });

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 7);
}

TEST(retry, with_policy_leaves_the_original_description_alone) {
    std::vector<ms> original_delays;
    std::vector<ms> derived_delays;

    auto original = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{2}}, .delay = 100ms},
        []() -> opt_string { return std::nullopt; },
        {
            .delay = [&original_delays](ms delay) { original_delays.push_back(delay); },
        });

    auto derived =
        original
            .with_policy(policy::constant{.transformers = {policy::transformer::limit_attempts{2}},
                                          .delay = 900ms})
            .with_delay([&derived_delays](ms delay) { derived_delays.push_back(delay); });

    EXPECT_FALSE(original());
    EXPECT_FALSE(derived());

    EXPECT_EQ(original_delays, (std::vector<ms>{100ms}));
    EXPECT_EQ(derived_delays, (std::vector<ms>{900ms}));
}

TEST(retry, with_check_narrows_what_counts_as_success) {
    int attempts = 0;

    auto counting = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 0ms},
        [&attempts]() -> opt_string {
            attempts++;
            return "short";
        },
        {
            .delay = [](ms) {},
        });

    // The default check is happy with any engaged optional.
    ASSERT_TRUE(counting());
    ASSERT_EQ(attempts, 1);

    attempts = 0;

    auto picky =
        counting.with_check([](const opt_string& value) { return value && value->size() > 10; });

    EXPECT_FALSE(picky());
    EXPECT_EQ(attempts, 4);
}

TEST(delay_t, constant) {
    ms waited_time{0};
    int attempts{0};

    crn::delay_t delay([&waited_time, &attempts](ms time) {
        waited_time += time;
        attempts += 1;
    });

    auto status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(waited_time, policy::detail::DEFAULT_DELAY);

    status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, 2 * policy::detail::DEFAULT_DELAY);
}

TEST(delay_t, constant_with_limit) {
    ms waited_time{0};
    int attempts{0};

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
    };

    crn::delay_t delay(policy, [&waited_time, &attempts](ms time) {
        waited_time += time;
        attempts += 1;
    });

    auto status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(waited_time, policy.delay);

    status = delay();

    ASSERT_FALSE(status);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(waited_time, policy.delay);
}

TEST(delay_t, exponential_with_limit) {
    ms waited_time{0};
    int attempts{0};

    policy::exponential policy{
        .transformers = {policy::transformer::limit_attempts{3}},
        .base = 500ms,
    };

    crn::delay_t delay(policy, [&waited_time, &attempts](ms time) {
        waited_time += time;
        attempts += 1;
    });

    auto status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(waited_time, policy.base);

    status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, policy.base + policy.base * 2);

    status = delay();

    ASSERT_FALSE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, policy.base + policy.base * 2);
}

TEST(delay_t, reset) {
    ms waited_time{0};
    int attempts{0};

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
    };

    crn::delay_t delay(policy, [&waited_time, &attempts](ms time) {
        waited_time += time;
        attempts += 1;
    });

    auto status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(waited_time, policy.delay);

    delay.reset();
    status = delay();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, 2 * policy.delay);
}

// --- regressions -----------------------------------------------------------

TEST(retry, cap_delay_survives_a_later_transformer) {
    // Transformers used to overwrite each other instead of composing: each one
    // returned a fresh transformation and discarded the accumulated one. A cap
    // placed before any other transformer was therefore silently dropped, and
    // the result depended on the order of the vector.
    const ms cap{1000};
    int cap_reached = 0;
    int below_cap = 0;

    std::vector<policy::transformer_t> transformers{
        policy::transformer::cap_delay{cap},
        policy::transformer::limit_attempts{8},
    };

    policy::exponential policy{
        .transformers = transformers,
        .base = 200ms,
    };

    auto result = crn::make_retry(policy, []() -> opt_string { return std::nullopt; },
                                  {
                                      .delay =
                                          [cap, &cap_reached, &below_cap](ms delay) {
                                              if(delay == cap)
                                                  cap_reached++;
                                              else
                                                  below_cap++;
                                          },
                                  })();

    ASSERT_FALSE(result);

    // Same numbers as fail_exponential_policy_with_transformer_cap_delay, which
    // declares the two transformers the other way round.
    EXPECT_EQ(cap_reached, 3);
    EXPECT_EQ(below_cap, 4);
}

TEST(retry, limit_attempts_below_one_stops_immediately) {
    // The limit was compared with != instead of >=, so any value the iteration
    // counter could never equal meant "retry forever".
    int attempts = 0;
    int delays = 0;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{0}},
        .delay = 0ms,
    };

    auto result = crn::make_retry(policy,
                                  [&attempts]() -> opt_string {
                                      attempts++;
                                      return std::nullopt;
                                  },
                                  {
                                      .delay = [&delays](ms) { delays++; },
                                  })();

    ASSERT_FALSE(result);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(delays, 0);
}

TEST(apply_transformers, cap_delay_without_a_previous_delay) {
    // cap_delay dereferenced status_t::previous_delay unguarded, while every
    // other transformer checked it first.
    const detail::status_t status{
        .curr_iteration = 0,
        .cumulative_delay = 0ms,
        .previous_delay = std::nullopt,
    };

    const std::vector<policy::transformer_t> transformers{
        policy::transformer::cap_delay{1000ms},
    };

    EXPECT_EQ(apply_transformers(transformers, status, 200ms), std::optional{200ms});
}

TEST(apply_transformers, a_spent_transformer_short_circuits_the_rest) {
    const detail::status_t status{};

    const std::vector<policy::transformer_t> transformers{
        policy::transformer::limit_attempts{0},
        policy::transformer::cap_delay{1ms},  // would rewrite the delay
    };

    EXPECT_EQ(apply_transformers(transformers, status, 200ms), std::nullopt);
}

TEST(retry, a_retry_whose_action_always_throws_can_be_run_twice) {
    // Every attempt throwing used to leave the result optional empty, and the
    // second call read it without checking: asking a failed retry for its
    // outcome twice threw std::bad_optional_access from inside the library.
    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
        .delay = 0ms,
    };

    auto retrying =
        crn::make_retry(policy, []() -> opt_string { throw std::runtime_error("always fails"); },
                        {
                            .delay = [](ms) {},
                        });

    EXPECT_FALSE(retrying());
    EXPECT_NO_THROW((void)retrying());
    EXPECT_FALSE(retrying());
}

TEST(exponential_backoff, saturates_instead_of_overflowing) {
    // base * pow(2, iteration) was computed in int: past roughly thirty
    // iterations the cast back was undefined behaviour.
    EXPECT_EQ(detail::exponential_backoff(200ms, 0), 200ms);
    EXPECT_EQ(detail::exponential_backoff(200ms, 3), 1600ms);
    EXPECT_GT(detail::exponential_backoff(200ms, 1000), ms{0});
    EXPECT_EQ(detail::exponential_backoff(200ms, 1000), detail::exponential_backoff(200ms, 2000));
}

// --- recovering ------------------------------------------------------------

namespace {
struct transient_t : std::runtime_error {
    transient_t() : std::runtime_error("try again") {}
};

struct permanent_t : std::runtime_error {
    permanent_t() : std::runtime_error("do not try again") {}
};
}  // namespace

TEST(recovering, by_default_every_exception_is_worth_another_attempt) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{3}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            throw permanent_t();
        },
        {.delay = [](ms) {}})();

    ASSERT_FALSE(result);
    EXPECT_EQ(calls, 3);
}

TEST(recovering, recover_only_rethrows_what_it_does_not_recognise) {
    int calls = 0;

    auto retrying = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{5}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            throw permanent_t();
        },
        {.recover = crn::retry::recover_only<transient_t>(), .delay = [](ms) {}});

    EXPECT_THROW(std::ignore = retrying(), permanent_t);
    EXPECT_EQ(calls, 1);
}

TEST(recovering, recover_only_keeps_retrying_what_it_does_recognise) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            throw transient_t();
        },
        {.recover = crn::retry::recover_only<transient_t>(), .delay = [](ms) {}})();

    ASSERT_FALSE(result);
    EXPECT_EQ(calls, 4);
    EXPECT_TRUE(result.error().cause);
}

TEST(recovering, recover_except_is_the_other_way_round) {
    int calls = 0;

    auto retrying = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{5}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            throw permanent_t();
        },
        {.recover = crn::retry::recover_except<permanent_t>(), .delay = [](ms) {}});

    EXPECT_THROW(std::ignore = retrying(), permanent_t);
    EXPECT_EQ(calls, 1);
}

TEST(recovering, a_base_class_counts_as_a_match) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{3}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            throw transient_t();
        },
        {.recover = crn::retry::recover_only<std::runtime_error>(), .delay = [](ms) {}})();

    ASSERT_FALSE(result);
    EXPECT_EQ(calls, 3);
}

TEST(recovering, recover_none_gives_up_on_the_first_throw) {
    int calls = 0;

    auto retrying = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{5}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            throw transient_t();
        },
        {.recover = crn::retry::recover_none, .delay = [](ms) {}});

    EXPECT_THROW(std::ignore = retrying(), transient_t);
    EXPECT_EQ(calls, 1);
}

// --- transient -------------------------------------------------------------

namespace {
struct http_error_t {
    int status;
};

using response_t = std::expected<std::string, http_error_t>;
}  // namespace

TEST(transient, by_default_every_failed_result_is_worth_another_attempt) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 0ms},
        [&calls]() -> opt_string {
            calls++;
            return std::nullopt;
        },
        {.delay = [](ms) {}})();

    ASSERT_FALSE(result);
    EXPECT_EQ(calls, 4);
}

TEST(transient, a_permanent_failure_stops_the_sequence_at_once) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{10}}, .delay = 0ms},
        [&calls]() -> response_t {
            calls++;
            return std::unexpected(http_error_t{404});
        },
        {.transient =
             crn::retry::on_error([](const http_error_t& error) { return error.status >= 500; }),
         .delay = [](ms) {}})();

    EXPECT_EQ(calls, 1);

    // The retry reached an answer, so the outer expected holds a value. The
    // answer itself is the failure the action reported.
    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value());
    EXPECT_EQ(result.value().error().status, 404);
}

TEST(transient, a_temporary_failure_is_still_retried) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 0ms},
        [&calls]() -> response_t {
            calls++;
            return std::unexpected(http_error_t{503});
        },
        {.transient =
             crn::retry::on_error([](const http_error_t& error) { return error.status >= 500; }),
         .delay = [](ms) {}})();

    EXPECT_EQ(calls, 4);

    // Here the policy ran out, which is the other kind of ending.
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().attempts, 4);
}

TEST(transient, a_success_is_never_asked_about) {
    int calls = 0;
    int asked = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 0ms},
        [&calls]() -> response_t {
            if(++calls < 3) return std::unexpected(http_error_t{503});

            return "ready";
        },
        {.transient =
             [&asked](const response_t& response) {
                 asked++;
                 return response.error().status >= 500;
             },
         .delay = [](ms) {}})();

    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value());
    EXPECT_EQ(result.value().value(), "ready");
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(asked, 2) << "only the two failures were judged";
}

namespace {
struct timed_out_t {};
struct throttled_t {};
struct not_found_t {};

using fault_t = std::variant<timed_out_t, throttled_t, not_found_t>;
using fetched_t = std::expected<std::string, fault_t>;
}  // namespace

TEST(transient, only_these_alternatives_are_worth_retrying) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{10}}, .delay = 0ms},
        [&calls]() -> fetched_t {
            calls++;
            return std::unexpected(not_found_t{});
        },
        {.transient = crn::retry::transient_only<timed_out_t, throttled_t>(),
         .delay = [](ms) {}})();

    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value());
    EXPECT_TRUE(std::holds_alternative<not_found_t>(result.value().error()));
}

TEST(transient, a_listed_alternative_keeps_being_retried) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 0ms},
        [&calls]() -> fetched_t {
            calls++;
            return std::unexpected(throttled_t{});
        },
        {.transient = crn::retry::transient_only<timed_out_t, throttled_t>(),
         .delay = [](ms) {}})();

    EXPECT_EQ(calls, 4);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().attempts, 4);
}

TEST(transient, except_is_the_other_way_round) {
    int calls = 0;

    auto result = crn::make_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{10}}, .delay = 0ms},
        [&calls]() -> fetched_t {
            calls++;
            return std::unexpected(not_found_t{});
        },
        {.transient = crn::retry::transient_except<not_found_t>(), .delay = [](ms) {}})();

    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value());
}
