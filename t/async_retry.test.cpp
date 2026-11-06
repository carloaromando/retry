#undef NDEBUG
#include <cassert>
#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <retry/async_retry.hpp>
#include <retry/retry.hpp>

using namespace crn::retry;
using namespace std::chrono_literals;

using opt_string = std::optional<std::string>;
using ms = crn::retry::duration_t;

TEST(async_delay_t, constant)
{
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void>
        {
            crn::async_delay_t delay(
                [&waited_time, &attempts](ms time) -> asio::awaitable<void>
                {
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

TEST(async_delay_t, constant_with_limit)
{
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void>
        {
            crn::async_delay_t delay(
                policy,
                [&waited_time, &attempts](ms time) -> asio::awaitable<void>
                {
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

TEST(async_delay_t, exponential_with_limit)
{
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
        [&]() -> asio::awaitable<void>
        {
            crn::async_delay_t delay(
                policy,
                [&waited_time, &attempts](ms time) -> asio::awaitable<void>
                {
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

TEST(async_delay_t, reset)
{
    ms waited_time{0};
    int attempts{0};
    bool status{false};
    asio::io_context io_context;

    policy::constant policy{
        .transformers = {policy::transformer::limit_attempts{2}},
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void>
        {
            crn::async_delay_t delay(
                policy,
                [&waited_time, &attempts](ms time) -> asio::awaitable<void>
                {
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

TEST(async_delay_t, ringed_exponential_with_jitter)
{
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
        .seeder = []() -> std::seed_seq
        {
            return {0};
        },
        .restart_after = 3,
    };

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void>
        {
            crn::async_delay_t delay(
                policy,
                [&waited](ms time) -> asio::awaitable<void>
                {
                    waited.push_back(time);
                    co_return;
                });

            for(int i = 0; i < 4; ++i)
                status = co_await delay();
        },
        asio::detached);

    io_context.run();

    ASSERT_TRUE(status);

    // The same sequence the blocking front end produces for this policy, see
    // retry.ringed_exponential_with_jitter_policy.
    EXPECT_EQ(waited, (std::vector<ms>{449ms, 961ms, 1985ms, 147ms}));
}
