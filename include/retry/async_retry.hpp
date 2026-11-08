#pragma once

#include <asio/awaitable.hpp>
#include <asio/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "retry.hpp"

namespace crn {
namespace retry {
using async_delay_function_t = std::function<asio::awaitable<void>(retry::duration_t delay)>;

namespace detail {
template <typename T>
struct awaitable_value {};

template <typename T, typename E>
struct awaitable_value<asio::awaitable<T, E>> {
    using type = T;
};

template <typename T>
using awaitable_value_t = typename awaitable_value<T>::type;
}  // namespace detail

// An action whose call returns an asio::awaitable.
template <typename F>
concept async_action = std::invocable<F> && requires {
    typename retry::detail::awaitable_value_t<std::invoke_result_t<F>>;
};

inline auto default_async_delay_function(retry::duration_t delay) -> asio::awaitable<void> {
    co_await asio::steady_timer(co_await asio::this_coro::executor, delay)
        .async_wait(asio::use_awaitable);
}

// The coroutine counterpart of retry::delay, down to the shared next_delay: a
// policy produces the same sequence whichever of the two drives it.
inline auto async_delay(retry::policy_t policy, retry::detail::status_t prev,
                        retry::async_delay_function_t delay_function)
    -> asio::awaitable<std::optional<retry::detail::status_t>> {
    auto delay = retry::next_delay(policy, prev);

    if(!delay) co_return std::nullopt;

    auto next = retry::detail::advance(prev, delay.value());

    co_await delay_function(next.previous_delay.value_or(retry::duration_t{0}));

    co_return next;
}

namespace detail {
inline auto is_cancellation(const std::exception_ptr& cause) -> bool {
    if(!cause) return false;

    try {
        std::rethrow_exception(cause);
    } catch(const asio::system_error& error) {
        return error.code() == asio::error::operation_aborted;
    } catch(...) {
        return false;
    }
}
}  // namespace detail

// Cancellation is not a failure to retry. Swallowing it and trying again is
// never what the caller asked for, so it is the one thing the asynchronous
// default refuses to recover from.
inline const retry::recover_function_t recover_all_but_cancellation =
    [](const std::exception_ptr& cause) { return !retry::detail::is_cancellation(cause); };

template <typename T>
struct async_params_t {
    using check_function_t = std::function<bool(const T&)>;

    using transient_function_t = std::function<bool(const T&)>;

    check_function_t check{retry::params_t<T>::DEFAULT_CHECK};
    transient_function_t transient{retry::params_t<T>::DEFAULT_TRANSIENT};
    retry::recover_function_t recover{retry::recover_all_but_cancellation};
    retry::async_delay_function_t delay{[](retry::duration_t delay) -> asio::awaitable<void> {
        co_await retry::default_async_delay_function(delay);
    }};
};
}  // namespace retry

// The coroutine counterpart of retry_t, with the same contract: building does
// nothing, co_awaiting it runs it, and running it leaves it unchanged.
template <typename F, typename T = retry::detail::awaitable_value_t<std::invoke_result_t<F>>>
    requires retry::async_action<F> && (!std::is_void_v<T>)
class async_retry_t {
    retry::policy_t policy_;
    retry::async_params_t<T> params_;
    F action_;

    // Everything the loop needs arrives by value, so the coroutine frame owns
    // its state and cannot outlive a temporary description.
    static auto run_(retry::policy_t policy, retry::async_params_t<T> params, F action)
        -> asio::awaitable<retry::result_t<T>> {
        retry::detail::status_t status{};
        retry::error_t error{};

        while(true) {
            try {
                auto result = co_await std::invoke(action);

                error.attempts += 1;

                if(params.check(result)) co_return result;

                if(!params.transient(result)) co_return result;

                if(!params.transient(result)) co_return result;
            } catch(...) {
                auto cause = std::current_exception();

                if(!params.recover(cause)) std::rethrow_exception(cause);

                error.attempts += 1;
                error.cause = cause;
            }

            // A loop rather than the recursion its blocking twin uses: each
            // recursive step here would be a coroutine frame kept alive until
            // the innermost attempt returns.
            auto next = co_await retry::async_delay(policy, status, params.delay);

            if(!next) co_return std::unexpected(std::move(error));

            error.waited += next->previous_delay.value_or(retry::duration_t{0});
            status = next.value();
        }
    }

public:
    explicit async_retry_t(F action, retry::async_params_t<T> params = retry::async_params_t<T>{})
        : policy_(retry::policy::full_jitter{}),
          params_(std::move(params)),
          action_(std::move(action)) {}

    async_retry_t(retry::policy_t policy, F action,
                  retry::async_params_t<T> params = retry::async_params_t<T>{})
        : policy_(std::move(policy)), params_(std::move(params)), action_(std::move(action)) {}

    [[nodiscard]] auto operator()() const -> asio::awaitable<retry::result_t<T>> {
        return run_(this->policy_, this->params_, this->action_);
    }

    template <typename Self>
    [[nodiscard]] auto with_policy(this Self&& self, retry::policy_t policy) -> async_retry_t {
        auto next = std::forward<Self>(self);
        next.policy_ = std::move(policy);

        return next;
    }

    template <typename Self>
    [[nodiscard]] auto with_check(this Self&& self,
                                  typename retry::async_params_t<T>::check_function_t check)
        -> async_retry_t {
        auto next = std::forward<Self>(self);
        next.params_.check = std::move(check);

        return next;
    }

    template <typename Self>
    [[nodiscard]] auto with_delay(this Self&& self, retry::async_delay_function_t delay)
        -> async_retry_t {
        auto next = std::forward<Self>(self);
        next.params_.delay = std::move(delay);

        return next;
    }
};

template <typename F, typename T = retry::detail::awaitable_value_t<std::invoke_result_t<F>>>
    requires retry::async_action<F>
[[nodiscard]] auto make_async_retry(F func) -> async_retry_t<F, T> {
    return async_retry_t<F, T>{std::move(func)};
}

template <typename F, typename T = retry::detail::awaitable_value_t<std::invoke_result_t<F>>>
    requires retry::async_action<F>
[[nodiscard]] auto make_async_retry(F func, retry::async_params_t<T> params)
    -> async_retry_t<F, T> {
    return async_retry_t<F, T>{std::move(func), std::move(params)};
}

template <typename F, typename T = retry::detail::awaitable_value_t<std::invoke_result_t<F>>>
    requires retry::async_action<F>
[[nodiscard]] auto make_async_retry(retry::policy_t policy, F func) -> async_retry_t<F, T> {
    return async_retry_t<F, T>{std::move(policy), std::move(func)};
}

template <typename F, typename T = retry::detail::awaitable_value_t<std::invoke_result_t<F>>>
    requires retry::async_action<F>
[[nodiscard]] auto make_async_retry(retry::policy_t policy, F func, retry::async_params_t<T> params)
    -> async_retry_t<F, T> {
    return async_retry_t<F, T>{std::move(policy), std::move(func), std::move(params)};
}

// Backoff on its own, for coroutine loops written by hand. Like delay_t and
// unlike async_retry_t, this one is deliberately stateful.
class async_delay_t {
    retry::detail::status_t status_{};
    retry::policy_t policy_ = retry::policy::constant{};
    retry::async_delay_function_t delay_function_ =
        [](retry::duration_t delay) -> asio::awaitable<void> {
        co_await retry::default_async_delay_function(delay);
    };

public:
    async_delay_t() = default;
    explicit async_delay_t(retry::policy_t policy) : policy_(std::move(policy)) {}
    explicit async_delay_t(retry::async_delay_function_t delay_function)
        : delay_function_(std::move(delay_function)) {}
    async_delay_t(retry::policy_t policy, retry::async_delay_function_t delay_function)
        : policy_(std::move(policy)), delay_function_(std::move(delay_function)) {}

    auto operator()() -> asio::awaitable<bool> {
        auto status =
            co_await retry::async_delay(this->policy_, this->status_, this->delay_function_);

        if(!status) co_return false;

        this->status_ = status.value();

        co_return true;
    }

    auto reset() -> void { this->status_ = {}; }
};
}  // namespace crn
