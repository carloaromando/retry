#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

#include <retry/retry.hpp>

using namespace crn::retry;
using namespace std::chrono_literals;

using ms = crn::retry::duration_t;

namespace
{
    constexpr int CASES = 200;

    // Fixed seed: a failing property reports the same counterexample on the
    // next run, which is the only way one is worth debugging.
    auto generator() -> std::mt19937
    {
        return std::mt19937{20240216U};
    }

    auto any_int(std::mt19937& rng, int low, int high) -> int
    {
        return std::uniform_int_distribution<int>{low, high}(rng);
    }

    auto any_delay(std::mt19937& rng) -> ms
    {
        return ms{any_int(rng, 0, 5000)};
    }

    auto any_status(std::mt19937& rng) -> detail::status_t
    {
        return {
            .curr_iteration = any_int(rng, 0, 20),
            .cumulative_delay = any_delay(rng),
            .previous_delay = any_delay(rng),
        };
    }

    // One of each limiting transformer, in a random order. None of them reads
    // the incoming delay, so none of them can rewrite it.
    auto any_limits(std::mt19937& rng) -> std::vector<policy::transformer_t>
    {
        std::vector<policy::transformer_t> transformers{
            policy::transformer::limit_attempts{any_int(rng, 1, 20)},
            policy::transformer::limit_by_delay{any_delay(rng)},
            policy::transformer::limit_by_cumulative_delay{any_delay(rng)},
        };

        std::shuffle(transformers.begin(), transformers.end(), rng);

        return transformers;
    }
} // namespace

TEST(property, an_empty_transformer_list_leaves_the_delay_alone)
{
    auto rng = generator();

    for(int i = 0; i < CASES; ++i)
    {
        auto status = any_status(rng);
        auto delay = any_delay(rng);

        EXPECT_EQ(apply_transformers({}, status, delay), std::optional{delay});
    }
}

TEST(property, the_order_of_the_transformers_does_not_matter)
{
    // Holds because at most one transformer rewrites the delay. Two caps in
    // one list would not commute: each overwrites the other's value, so the
    // last one to fire would win.
    auto rng = generator();

    for(int i = 0; i < CASES; ++i)
    {
        auto status = any_status(rng);
        auto delay = any_delay(rng);

        auto transformers = any_limits(rng);
        transformers.push_back(policy::transformer::cap_delay{any_delay(rng)});

        auto expected = apply_transformers(transformers, status, delay);

        for(int shuffle = 0; shuffle < 5; ++shuffle)
        {
            std::shuffle(transformers.begin(), transformers.end(), rng);

            EXPECT_EQ(apply_transformers(transformers, status, delay), expected) << "case " << i << ", shuffle " << shuffle;
        }
    }
}

TEST(property, limit_attempts_bounds_how_often_the_action_runs)
{
    auto rng = generator();

    for(int i = 0; i < CASES; ++i)
    {
        auto limit = any_int(rng, -3, 20);
        int calls = 0;

        auto result = crn::make_retry(
            policy::constant{
                .transformers = {policy::transformer::limit_attempts{limit}},
                .delay = 0ms,
            },
            [&calls]() -> std::optional<int>
            {
                calls++;
                return std::nullopt;
            },
            {
                .delay = [](ms) {},
            })();

        ASSERT_FALSE(result);

        // One attempt always happens: the limit governs the waits between
        // attempts, not whether the first one is made at all.
        EXPECT_EQ(calls, std::max(limit, 1)) << "limit " << limit;
        EXPECT_EQ(result.error().attempts, calls);
    }
}

TEST(property, the_cumulative_delay_accounts_for_every_wait_but_the_last)
{
    auto rng = generator();

    for(int i = 0; i < CASES; ++i)
    {
        policy::exponential policy{
            .transformers = {policy::transformer::limit_attempts{any_int(rng, 2, 12)}},
            .base = ms{any_int(rng, 1, 500)},
        };

        std::vector<ms> waited;
        detail::status_t status{};

        while(auto next = crn::retry::delay(
                  policy,
                  status,
                  [&waited](ms delay)
                  {
                      waited.push_back(delay);
                  }))
        {
            status = next.value();

            auto total = std::accumulate(waited.begin(), waited.end(), ms{0});

            EXPECT_EQ(status.cumulative_delay + status.previous_delay.value(), total);
            EXPECT_GE(status.cumulative_delay, ms{0});
        }
    }
}

TEST(property, exponential_backoff_never_shrinks_and_never_overflows)
{
    auto rng = generator();

    for(int i = 0; i < CASES; ++i)
    {
        auto base = any_delay(rng);
        auto previous = detail::exponential_backoff(base, 0);

        EXPECT_EQ(previous, base);

        for(int iteration = 1; iteration <= 80; ++iteration)
        {
            auto current = detail::exponential_backoff(base, iteration);

            EXPECT_GE(current, previous) << "base " << base.count() << ", iteration " << iteration;
            EXPECT_GE(current, ms{0});

            previous = current;
        }
    }
}

TEST(property, a_jittered_delay_stays_within_its_exponential_bound)
{
    auto rng = generator();

    for(int i = 0; i < CASES; ++i)
    {
        auto seed = static_cast<std::uint32_t>(any_int(rng, 0, 1000000));

        policy::full_jitter policy{
            .base = ms{any_int(rng, 1, 500)},
            .seeder = [seed]() -> std::seed_seq
            {
                return {seed};
            },
        };

        detail::status_t status{.curr_iteration = any_int(rng, 0, 15)};

        auto bound = detail::exponential_backoff(policy.base, status.curr_iteration);
        auto delay = detail::untransformed_delay(policy, status);

        EXPECT_GE(delay, ms{0});
        EXPECT_LE(delay, bound) << "seed " << seed << ", iteration " << status.curr_iteration;
    }
}
