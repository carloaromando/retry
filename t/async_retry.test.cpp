#undef NDEBUG
#include <gtest/gtest.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <cassert>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <retry/async_retry.hpp>
#include <retry/retry.hpp>

using namespace crn::retry;
using namespace std::chrono_literals;

using opt_string = std::optional<std::string>;
using ms = crn::retry::duration_t;

TEST(async_delay_t, constant) {
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            crn::async_delay_t delay([&waited_time, &attempts](ms time) -> asio::awaitable<void> {
                waited_time += time;
                attempts += 1;
                co_return;
            });

            status = co_await delay();

            assert(status);
            assert(attempts == 1);
            assert(waited_time == policy::detail::DEFAULT_DELAY);

            status = co_await delay();
        },
        asio::detached);

    io_context.run();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, 2 * policy::detail::DEFAULT_DELAY);
}

TEST(async_delay_t, constant_with_limit) {
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            crn::async_delay_t delay(policy,
                                     [&waited_time, &attempts](ms time) -> asio::awaitable<void> {
                                         waited_time += time;
                                         attempts += 1;
                                         co_return;
                                     });

            status = co_await delay();

            assert(status);
            assert(attempts == 1);
            assert(waited_time == policy.delay);

            status = co_await delay();
        },
        asio::detached);

    io_context.run();

    ASSERT_FALSE(status);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(waited_time, policy.delay);
}

TEST(async_delay_t, exponential_with_limit) {
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    policy::exponential policy{
        .transformers = {policy::transformer::limit_attempts{3}},
        .base = 500ms,
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            crn::async_delay_t delay(policy,
                                     [&waited_time, &attempts](ms time) -> asio::awaitable<void> {
                                         waited_time += time;
                                         attempts += 1;
                                         co_return;
                                     });

            status = co_await delay();

            assert(status);
            assert(attempts == 1);
            assert(waited_time == policy.base);

            status = co_await delay();

            assert(status);
            assert(attempts == 2);
            assert(waited_time == policy.base + policy.base * 2);

            status = co_await delay();
        },
        asio::detached);

    io_context.run();

    ASSERT_FALSE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, policy.base + policy.base * 2);
}

TEST(async_delay_t, reset) {
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            crn::async_delay_t delay(policy,
                                     [&waited_time, &attempts](ms time) -> asio::awaitable<void> {
                                         waited_time += time;
                                         attempts += 1;
                                         co_return;
                                     });

            status = co_await delay();

            assert(status);
            assert(attempts == 1);
            assert(waited_time == policy.delay);

            delay.reset();
            status = co_await delay();
        },
        asio::detached);

    io_context.run();

    ASSERT_TRUE(status);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(waited_time, 2 * policy.delay);
}

TEST(async_delay_t, ringed_exponential_with_jitter) {
    // async_delay reimplemented the policy dispatch with a chain of
    // holds_alternative and covered only three of the four policies, so a
    // ringed policy fell through every branch and produced no delay at all:
    // the very first call reported the sequence as exhausted.
    std::vector<ms> waited;
    bool status{false};
    asio::io_context io_context;

    policy::ringed_exponential_with_jitter policy{
        .transformers = {policy::transformer::limit_attempts{10}},
        .base = 500ms,
        .seeder = []() -> std::seed_seq { return {0}; },
        .restart_after = 3,
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            crn::async_delay_t delay(policy, [&waited](ms time) -> asio::awaitable<void> {
                waited.push_back(time);
                co_return;
            });

            for(int i = 0; i < 4; ++i) status = co_await delay();
        },
        asio::detached);

    io_context.run();

    ASSERT_TRUE(status);

    // The same sequence the blocking front end produces for this policy, see
    // retry.ringed_exponential_with_jitter_policy.
    EXPECT_EQ(waited, (std::vector<ms>{449ms, 961ms, 1985ms, 147ms}));
}

// --- async_retry_t ---------------------------------------------------------

namespace {
auto run(asio::io_context& io, auto body) -> void {
    asio::co_spawn(io, std::move(body), asio::detached);
    io.run();
}

auto recording(std::vector<ms>& waited) -> crn::retry::async_delay_function_t {
    return [&waited](ms delay) -> asio::awaitable<void> {
        waited.push_back(delay);
        co_return;
    };
}
}  // namespace

TEST(async_retry, nothing_runs_until_the_description_is_awaited) {
    asio::io_context io;
    int calls = 0;
    int calls_before_await = -1;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{1}}, .delay = 0ms},
        [&calls]() -> asio::awaitable<opt_string> {
            calls++;
            co_return "ready";
        },
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> {
        calls_before_await = calls;
        co_await retrying();
    });

    EXPECT_EQ(calls_before_await, 0);
    EXPECT_EQ(calls, 1);
}

TEST(async_retry, succeeds_after_a_few_failures) {
    asio::io_context io;
    int calls = 0;
    std::optional<crn::retry_result<opt_string>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{5}}, .delay = 0ms},
        [&calls]() -> asio::awaitable<opt_string> {
            if(++calls < 3) co_return std::nullopt;

            co_return "ready";
        },
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> { result = co_await retrying(); });

    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value());
    EXPECT_EQ(result.value().value().value(), "ready");
    EXPECT_EQ(calls, 3);
}

TEST(async_retry, gives_up_and_reports_what_it_spent) {
    asio::io_context io;
    std::optional<crn::retry_result<opt_string>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{4}}, .delay = 250ms},
        []() -> asio::awaitable<opt_string> { co_return std::nullopt; },
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> { result = co_await retrying(); });

    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value());
    EXPECT_EQ(result->error().attempts, 4);
    EXPECT_EQ(result->error().waited, 3 * 250ms);
    EXPECT_FALSE(result->error().cause);
}

TEST(async_retry, a_throwing_action_reports_the_last_exception) {
    asio::io_context io;
    std::optional<crn::retry_result<opt_string>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{3}}, .delay = 0ms},
        []() -> asio::awaitable<opt_string> {
            throw std::runtime_error("the server said no");
            co_return std::nullopt;
        },
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> { result = co_await retrying(); });

    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value());
    ASSERT_TRUE(result->error().cause);
    EXPECT_EQ(result->error().attempts, 3);

    try {
        std::rethrow_exception(result->error().cause);
        FAIL() << "expected the stored exception to be rethrown";
    } catch(const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "the server said no");
    }
}

TEST(async_retry, with_policy_leaves_the_original_description_alone) {
    asio::io_context io;
    std::vector<ms> original_delays;
    std::vector<ms> derived_delays;

    auto original = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{2}}, .delay = 100ms},
        []() -> asio::awaitable<opt_string> { co_return std::nullopt; },
        {.delay = recording(original_delays)});

    auto derived =
        original
            .with_policy(policy::constant{.transformers = {policy::transformer::limit_attempts{2}},
                                          .delay = 900ms})
            .with_delay(recording(derived_delays));

    run(io, [&]() -> asio::awaitable<void> {
        co_await original();
        co_await derived();
    });

    EXPECT_EQ(original_delays, (std::vector<ms>{100ms}));
    EXPECT_EQ(derived_delays, (std::vector<ms>{900ms}));
}

TEST(async_retry, produces_the_same_delays_as_its_blocking_twin) {
    // Both halves go through retry::next_delay, and this is what says so.
    policy::ringed_exponential_with_jitter shared{
        .transformers = {policy::transformer::limit_attempts{8}},
        .base = 500ms,
        .seeder = []() -> std::seed_seq { return {0}; },
        .restart_after = 3,
    };

    std::vector<ms> blocking;
    std::vector<ms> coroutine;

    auto sync_result =
        crn::make_retry(shared, []() -> opt_string { return std::nullopt; },
                        {.delay = [&blocking](ms delay) { blocking.push_back(delay); }})();

    asio::io_context io;
    std::optional<crn::retry_result<opt_string>> async_result;

    auto retrying = crn::make_async_retry(
        shared, []() -> asio::awaitable<opt_string> { co_return std::nullopt; },
        {.delay = recording(coroutine)});

    run(io, [&]() -> asio::awaitable<void> { async_result = co_await retrying(); });

    ASSERT_FALSE(sync_result);
    ASSERT_TRUE(async_result);
    ASSERT_FALSE(async_result.value());

    EXPECT_FALSE(blocking.empty());
    EXPECT_EQ(coroutine, blocking);
    EXPECT_EQ(async_result->error().attempts, sync_result.error().attempts);
    EXPECT_EQ(async_result->error().waited, sync_result.error().waited);
}
