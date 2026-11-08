#undef NDEBUG
#include <gtest/gtest.h>

#include <asio/awaitable.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <cassert>
#include <chrono>
#include <exception>
#include <expected>
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

struct http_error_t {
    int status;
};

using response_t = std::expected<std::string, http_error_t>;

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
    std::optional<crn::retry::result_t<opt_string>> result;

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
    std::optional<crn::retry::result_t<opt_string>> result;

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
    std::optional<crn::retry::result_t<opt_string>> result;

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
    std::optional<crn::retry::result_t<opt_string>> async_result;

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

// --- cancellation ----------------------------------------------------------

namespace {
auto cancellable(asio::io_context& io, asio::cancellation_signal& cancel, auto body) -> void {
    asio::co_spawn(io, std::move(body),
                   asio::bind_cancellation_slot(cancel.slot(), asio::detached));
}

auto aborted(const std::exception_ptr& cause) -> bool {
    try {
        std::rethrow_exception(cause);
    } catch(const asio::system_error& error) {
        return error.code() == asio::error::operation_aborted;
    } catch(...) {
        return false;
    }
}
}  // namespace

TEST(async_retry, cancelling_the_backoff_aborts_instead_of_retrying) {
    asio::io_context io;
    asio::cancellation_signal cancel;

    int attempts = 0;
    std::exception_ptr escaped;
    std::optional<crn::retry::result_t<opt_string>> result;

    // A real timer, so the cancellation lands while the backoff is waiting.
    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{20}}, .delay = 200ms},
        [&attempts]() -> asio::awaitable<opt_string> {
            attempts++;
            co_return std::nullopt;
        });

    cancellable(io, cancel, [&]() -> asio::awaitable<void> {
        try {
            result = co_await retrying();
        } catch(...) {
            escaped = std::current_exception();
        }
    });

    asio::steady_timer trigger(io, 300ms);
    trigger.async_wait([&cancel](auto) { cancel.emit(asio::cancellation_type::all); });

    io.run();

    EXPECT_FALSE(result) << "cancellation is not a retry outcome";
    ASSERT_TRUE(escaped);
    EXPECT_TRUE(aborted(escaped));

    // It stopped where it was cancelled rather than burning the whole policy.
    EXPECT_GT(attempts, 0);
    EXPECT_LT(attempts, 20);
}

TEST(async_retry, a_cancelled_action_is_not_swallowed_and_retried) {
    asio::io_context io;
    asio::cancellation_signal cancel;

    int attempts = 0;
    std::exception_ptr escaped;
    std::optional<crn::retry::result_t<opt_string>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{3}}, .delay = 0ms},
        [&attempts]() -> asio::awaitable<opt_string> {
            attempts++;

            // In flight when the cancellation arrives.
            co_await asio::steady_timer(co_await asio::this_coro::executor, 200ms)
                .async_wait(asio::use_awaitable);

            co_return std::nullopt;
        },
        // The wait between attempts returns at once, so it cannot be cancelled
        // itself. Without that, the backoff timer would abort the loop on its
        // own and hide whether the action's cancellation was swallowed.
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    cancellable(io, cancel, [&]() -> asio::awaitable<void> {
        try {
            result = co_await retrying();
        } catch(...) {
            escaped = std::current_exception();
        }
    });

    asio::steady_timer trigger(io, 50ms);
    trigger.async_wait([&cancel](auto) { cancel.emit(asio::cancellation_type::all); });

    io.run();

    // The default recover refuses cancellation: the action runs once and the
    // abort travels out, instead of being counted as a failed attempt.
    EXPECT_EQ(attempts, 1);
    EXPECT_FALSE(result);
    ASSERT_TRUE(escaped);
    EXPECT_TRUE(aborted(escaped));
}

TEST(async_retry, an_abort_surfaced_as_an_exception_is_not_retried_either) {
    // Asio's own cancellation never reaches the catch below: it unwinds the
    // coroutine chain by itself, as the test above shows. Code that turns a
    // cancelled operation into a thrown system_error -- redirect_error and a
    // manual throw, say -- is the case the default recover exists for.
    asio::io_context io;

    int attempts = 0;
    std::exception_ptr escaped;
    std::optional<crn::retry::result_t<opt_string>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{3}}, .delay = 0ms},
        [&attempts]() -> asio::awaitable<opt_string> {
            attempts++;
            throw asio::system_error(asio::error::operation_aborted);
            co_return std::nullopt;
        },
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> {
        try {
            result = co_await retrying();
        } catch(...) {
            escaped = std::current_exception();
        }
    });

    EXPECT_EQ(attempts, 1) << "an aborted operation must not be tried again";
    EXPECT_FALSE(result);
    ASSERT_TRUE(escaped);
    EXPECT_TRUE(aborted(escaped));
}

TEST(async_retry, other_asio_errors_are_still_worth_retrying) {
    // The default refuses cancellation alone, not every asio failure: a refused
    // connection is exactly the kind of thing this library exists for.
    asio::io_context io;

    int attempts = 0;
    std::optional<crn::retry::result_t<opt_string>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{3}}, .delay = 0ms},
        [&attempts]() -> asio::awaitable<opt_string> {
            attempts++;
            throw asio::system_error(asio::error::connection_refused);
            co_return std::nullopt;
        },
        {.delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> { result = co_await retrying(); });

    EXPECT_EQ(attempts, 3);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value());
}

TEST(async_retry, a_permanent_failure_stops_the_sequence_at_once) {
    asio::io_context io;
    int calls = 0;
    std::optional<crn::retry::result_t<response_t>> result;

    auto retrying = crn::make_async_retry(
        policy::constant{.transformers = {policy::transformer::limit_attempts{10}}, .delay = 0ms},
        [&calls]() -> asio::awaitable<response_t> {
            calls++;
            co_return std::unexpected(http_error_t{404});
        },
        {.transient =
             crn::retry::on_error([](const http_error_t& error) { return error.status >= 500; }),
         .delay = [](ms) -> asio::awaitable<void> { co_return; }});

    run(io, [&]() -> asio::awaitable<void> { result = co_await retrying(); });

    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value());
    ASSERT_FALSE(result->value());
    EXPECT_EQ(result->value().error().status, 404);
}
