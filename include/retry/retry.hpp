#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace crn
{
    namespace retry
    {
        // Every delay in this library is a duration, never a bare number. The
        // unit is part of the type, so nothing can be read as the wrong one.
        using duration_t = std::chrono::milliseconds;
        using seeder_t = std::function<std::seed_seq()>;

        namespace detail
        {
            template <typename... Fs> struct overloaded : Fs...
            {
                using Fs::operator()...;
            };

            template <typename... Fs> overloaded(Fs...) -> overloaded<Fs...>;

            // Types with an *explicit* operator bool -- optional, unique_ptr,
            // expected -- carry their own notion of failure. Types that convert
            // implicitly do not: an int of 0 is a perfectly good result, not a
            // failed attempt.
            template <typename T>
            concept explicitly_boolean = std::constructible_from<bool, T> && !std::convertible_to<T, bool>;

            struct status_t
            {
                int curr_iteration{0};
                duration_t cumulative_delay{0};
                std::optional<duration_t> previous_delay{duration_t{0}};
            };

            // base * 2^iteration, saturating rather than overflowing.
            inline auto exponential_backoff(duration_t base, int iteration) -> duration_t
            {
                using rep_t = duration_t::rep;

                constexpr auto MAX_EXPONENT = 62;
                constexpr auto MAX_REP = std::numeric_limits<rep_t>::max();

                auto factor = rep_t{1} << std::clamp(iteration, 0, MAX_EXPONENT);

                if(base.count() > MAX_REP / factor)
                    return duration_t{MAX_REP};

                return base * factor;
            }
        } // namespace detail

        namespace policy
        {
            namespace transformer
            {
                struct limit_attempts
                {
                    int value;
                };

                struct limit_by_delay
                {
                    retry::duration_t value;
                };

                struct limit_by_cumulative_delay
                {
                    retry::duration_t value;
                };

                struct cap_delay
                {
                    retry::duration_t value;
                };
            } // namespace transformer

            using transformer_t = std::variant<
                transformer::limit_attempts,
                transformer::limit_by_delay,
                transformer::limit_by_cumulative_delay,
                transformer::cap_delay>;

            namespace detail
            {
                inline constexpr auto DEFAULT_LIMIT = 5;
                inline constexpr auto DEFAULT_DELAY = retry::duration_t{1000};
                inline constexpr auto DEFAULT_BASE_DELAY = retry::duration_t{500};
                inline constexpr auto DEFAULT_RESTART = 10;

                // Seeding from the clock alone gives two retries in the same
                // tick the same "random" delay, which defeats the purpose of
                // jitter. random_device breaks the tie.
                inline constexpr auto DEFAULT_SEEDER = []() -> std::seed_seq
                {
                    std::random_device device;

                    auto time_seed = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());

                    return {device(), static_cast<std::uint32_t>(time_seed & 0xffffffffU), static_cast<std::uint32_t>(time_seed >> 32U)};
                };

            } // namespace detail

            // Flat aggregates on purpose. Sharing a base class would make every
            // call site mix designated and positional initialisers, and would
            // claim that a ringed policy is substitutable for a plain jittered
            // one, which it is not.
            struct constant
            {
                std::vector<transformer_t> transformers{transformer::limit_attempts{detail::DEFAULT_LIMIT}};
                retry::duration_t delay{detail::DEFAULT_DELAY};
            };

            struct exponential
            {
                std::vector<transformer_t> transformers{transformer::limit_attempts{detail::DEFAULT_LIMIT}};
                retry::duration_t base{detail::DEFAULT_BASE_DELAY};
            };

            struct full_jitter
            {
                std::vector<transformer_t> transformers{transformer::limit_attempts{detail::DEFAULT_LIMIT}};
                retry::duration_t base{detail::DEFAULT_BASE_DELAY};
                retry::seeder_t seeder{detail::DEFAULT_SEEDER};
            };

            struct ringed_exponential_with_jitter
            {
                std::vector<transformer_t> transformers{transformer::limit_attempts{detail::DEFAULT_LIMIT}};
                retry::duration_t base{detail::DEFAULT_BASE_DELAY};
                retry::seeder_t seeder{detail::DEFAULT_SEEDER};
                int restart_after{detail::DEFAULT_RESTART};
            };

        } // namespace policy

        using policy_t = std::variant<policy::constant, policy::exponential, policy::full_jitter, policy::ringed_exponential_with_jitter>;
        using delay_function_t = std::function<void(duration_t)>;

        template <typename T> struct params_t
        {
            static constexpr auto DEFAULT_CHECK = [](const T& result) -> bool
            {
                if constexpr(std::same_as<T, bool>)
                    return result;
                else if constexpr(retry::detail::explicitly_boolean<const T&>)
                    return static_cast<bool>(result);
                else
                    return true;
            };

            static constexpr auto DEFAULT_DELAY = [](retry::duration_t delay) -> void
            {
                std::this_thread::sleep_for(delay);
            };

            using check_function_t = std::function<bool(const T&)>;

            check_function_t check{DEFAULT_CHECK};
            delay_function_t delay{DEFAULT_DELAY};
        };

        namespace detail
        {
            // A uniform draw in [0, max], seeded by the policy. Shared by the
            // jittered policies instead of inherited from one of them.
            inline auto jittered(duration_t max, const retry::seeder_t& seeder) -> duration_t
            {
                using rng_t = std::mt19937;

                constexpr auto MAX_BOUND = std::numeric_limits<rng_t::result_type>::max();

                auto bound = static_cast<rng_t::result_type>(std::clamp<duration_t::rep>(max.count(), 0, MAX_BOUND));

                std::uniform_int_distribution<rng_t::result_type> udist(0, bound);

                auto seed_seq = seeder();
                rng_t rng{seed_seq};

                return duration_t{udist(rng)};
            }

            // The exponent restarts every restart_after attempts, so the delay
            // keeps growing without ever running away.
            inline auto ringed_iteration(int iteration, int restart_after) -> int
            {
                return iteration % (restart_after + 1);
            }

            // A transformer either leaves the delay alone, rewrites it, or ends
            // the sequence by returning nullopt.
            inline auto
            apply_transformer(const retry::policy::transformer_t& transformer, const retry::detail::status_t& prev, duration_t delay)
                -> std::optional<duration_t>
            {
                return std::visit(
                    retry::detail::overloaded{
                        [&prev, delay](const retry::policy::transformer::limit_attempts& limit) -> std::optional<duration_t>
                        {
                            if((prev.curr_iteration + 1) >= limit.value)
                                return std::nullopt;

                            return delay;
                        },
                        [&prev, delay](const retry::policy::transformer::limit_by_delay& by_delay) -> std::optional<duration_t>
                        {
                            if(prev.previous_delay && by_delay.value > prev.previous_delay.value())
                                return delay;

                            return std::nullopt;
                        },
                        [&prev, delay](
                            const retry::policy::transformer::limit_by_cumulative_delay& by_cumulative_delay) -> std::optional<duration_t>
                        {
                            if(by_cumulative_delay.value > prev.cumulative_delay)
                                return delay;

                            return std::nullopt;
                        },
                        [&prev, delay](const retry::policy::transformer::cap_delay& cap) -> std::optional<duration_t>
                        {
                            if(cap.value <= prev.previous_delay.value_or(duration_t{0}))
                                return cap.value;

                            return delay;
                        },
                    },
                    transformer);
            }

            // Duck-typed on the transformers member, so it stays total without
            // every policy having to share a base class.
            inline auto transformers_of(const retry::policy_t& policy) -> const std::vector<retry::policy::transformer_t>&
            {
                return std::visit(
                    [](const auto& policy) -> const std::vector<retry::policy::transformer_t>&
                    {
                        return policy.transformers;
                    },
                    policy);
            }

            // The delay a policy asks for, before any transformer gets a say.
            inline auto untransformed_delay(const retry::policy_t& policy, const retry::detail::status_t& prev) -> duration_t
            {
                return std::visit(
                    retry::detail::overloaded{
                        [](const retry::policy::constant& policy) -> duration_t
                        {
                            return policy.delay;
                        },
                        [&prev](const retry::policy::exponential& policy) -> duration_t
                        {
                            return retry::detail::exponential_backoff(policy.base, prev.curr_iteration);
                        },
                        [&prev](const retry::policy::full_jitter& policy) -> duration_t
                        {
                            return retry::detail::jittered(
                                retry::detail::exponential_backoff(policy.base, prev.curr_iteration), policy.seeder);
                        },
                        [&prev](const retry::policy::ringed_exponential_with_jitter& policy) -> duration_t
                        {
                            auto iteration = retry::detail::ringed_iteration(prev.curr_iteration, policy.restart_after);

                            return retry::detail::jittered(retry::detail::exponential_backoff(policy.base, iteration), policy.seeder);
                        },
                    },
                    policy);
            }

            inline auto advance(const retry::detail::status_t& prev, duration_t delay) -> retry::detail::status_t
            {
                return {
                    .curr_iteration = prev.curr_iteration + 1,
                    .cumulative_delay = prev.cumulative_delay + prev.previous_delay.value_or(duration_t{0}),
                    .previous_delay = delay,
                };
            }
        } // namespace detail

        // Threads `delay` through every transformer in order. Any transformer
        // that ends the sequence short-circuits the rest.
        inline auto apply_transformers(
            const std::vector<retry::policy::transformer_t>& transformers, const retry::detail::status_t& prev, duration_t delay)
            -> std::optional<duration_t>
        {
            return std::ranges::fold_left(
                transformers,
                std::optional<duration_t>{delay},
                [&prev](const std::optional<duration_t>& acc, const retry::policy::transformer_t& transformer)
                {
                    return acc.and_then(
                        [&](duration_t delay)
                        {
                            return retry::detail::apply_transformer(transformer, prev, delay);
                        });
                });
        }

        // How long to wait before the next attempt, or nullopt when the policy
        // is exhausted. Shared by the blocking and the coroutine front ends, so
        // that neither can forget a policy.
        inline auto next_delay(const retry::policy_t& policy, const retry::detail::status_t& prev) -> std::optional<duration_t>
        {
            return retry::apply_transformers(
                retry::detail::transformers_of(policy), prev, retry::detail::untransformed_delay(policy, prev));
        }

        inline auto delay(const retry::policy_t& policy, const retry::detail::status_t& prev, const retry::delay_function_t& delay_function)
            -> std::optional<retry::detail::status_t>
        {
            return retry::next_delay(policy, prev)
                .transform(
                    [&](duration_t delay)
                    {
                        auto next = retry::detail::advance(prev, delay);

                        delay_function(next.previous_delay.value_or(duration_t{0}));

                        return next;
                    });
        }
    } // namespace retry

    // What is known when a retry gives up. The cause is the exception thrown by
    // the last attempt, if the last attempt threw; it is null when the attempts
    // merely returned results the check rejected.
    struct retry_error
    {
        std::exception_ptr cause{};
        int attempts{0};
        retry::duration_t waited{0};
    };

    template <typename T> using retry_result = std::expected<T, retry_error>;

    // A retry_t is a *description* of a retried computation, not a running one.
    // Building it does nothing; calling it runs it and hands back a value.
    // Running it never changes it, so the same description can be invoked twice
    // and mean the same thing both times.
    template <typename F, typename T = std::invoke_result_t<F>>
        requires std::invocable<F> && (!std::is_void_v<T>)
    class retry_t
    {
        retry::policy_t policy_;
        retry::params_t<T> params_;
        F action_;

        auto attempt_(retry::detail::status_t status, retry_error error) const -> retry_result<T>
        {
            try
            {
                auto result = std::invoke(this->action_);

                error.attempts += 1;

                if(this->params_.check(result))
                    return result;
            }
            catch(...)
            {
                error.attempts += 1;
                error.cause = std::current_exception();
            }

            auto next = retry::delay(this->policy_, status, this->params_.delay);

            if(!next)
                return std::unexpected(std::move(error));

            error.waited += next->previous_delay.value_or(retry::duration_t{0});

            return this->attempt_(next.value(), std::move(error));
        }

    public:
        explicit retry_t(F action, retry::params_t<T> params = retry::params_t<T>{})
            : policy_(retry::policy::full_jitter{}),
              params_(std::move(params)),
              action_(std::move(action))
        {
        }

        retry_t(retry::policy_t policy, F action, retry::params_t<T> params = retry::params_t<T>{})
            : policy_(std::move(policy)),
              params_(std::move(params)),
              action_(std::move(action))
        {
        }

        [[nodiscard]] auto operator()() const -> retry_result<T> { return this->attempt_(retry::detail::status_t{}, retry_error{}); }

        // The with_ family returns a new description rather than mutating this
        // one. Deducing this lets a chain move through its own temporaries
        // instead of copying at every step.
        template <typename Self> [[nodiscard]] auto with_policy(this Self&& self, retry::policy_t policy) -> retry_t
        {
            auto next = std::forward<Self>(self);
            next.policy_ = std::move(policy);

            return next;
        }

        template <typename Self>
        [[nodiscard]] auto with_check(this Self&& self, typename retry::params_t<T>::check_function_t check) -> retry_t
        {
            auto next = std::forward<Self>(self);
            next.params_.check = std::move(check);

            return next;
        }

        template <typename Self> [[nodiscard]] auto with_delay(this Self&& self, retry::delay_function_t delay) -> retry_t
        {
            auto next = std::forward<Self>(self);
            next.params_.delay = std::move(delay);

            return next;
        }
    };

    template <typename F, typename T = std::invoke_result_t<F>>
        requires std::invocable<F>
    [[nodiscard]] auto make_retry(F func) -> retry_t<F, T>
    {
        return retry_t<F, T>{std::move(func)};
    }

    template <typename F, typename T = std::invoke_result_t<F>>
        requires std::invocable<F>
    [[nodiscard]] auto make_retry(F func, retry::params_t<T> params) -> retry_t<F, T>
    {
        return retry_t<F, T>{std::move(func), std::move(params)};
    }

    template <typename F, typename T = std::invoke_result_t<F>>
        requires std::invocable<F>
    [[nodiscard]] auto make_retry(retry::policy_t policy, F func) -> retry_t<F, T>
    {
        return retry_t<F, T>{std::move(policy), std::move(func)};
    }

    template <typename F, typename T = std::invoke_result_t<F>>
        requires std::invocable<F>
    [[nodiscard]] auto make_retry(retry::policy_t policy, F func, retry::params_t<T> params) -> retry_t<F, T>
    {
        return retry_t<F, T>{std::move(policy), std::move(func), std::move(params)};
    }

    // Backoff on its own, for loops written by hand. Unlike retry_t this one is
    // deliberately stateful: each call advances the sequence.
    class delay_t
    {
        retry::detail::status_t status_{};
        retry::policy_t policy_ = retry::policy::constant{};
        retry::delay_function_t delay_function_ = [](retry::duration_t delay)
        {
            std::this_thread::sleep_for(delay);
        };

    public:
        delay_t() = default;
        explicit delay_t(retry::policy_t policy) : policy_(std::move(policy)) {}
        explicit delay_t(retry::delay_function_t delay_function) : delay_function_(std::move(delay_function)) {}
        delay_t(retry::policy_t policy, retry::delay_function_t delay_function)
            : policy_(std::move(policy)),
              delay_function_(std::move(delay_function))
        {
        }

        auto operator()() -> bool
        {
            auto status = retry::delay(this->policy_, this->status_, this->delay_function_);

            if(!status)
                return false;

            this->status_ = status.value();

            return true;
        }

        auto reset() -> void { this->status_ = {}; }
    };
} // namespace crn
