#pragma once
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "retry.hpp"

namespace crn {
namespace retry {
using async_delay_function_t = std::function<asio::awaitable<void>(retry::duration_t delay)>;

struct async_delay_params {
    retry::policy_t policy;
    retry::detail::status_t status;
    retry::async_delay_function_t delay_function;
};

inline auto default_async_delay_function(retry::duration_t delay) -> asio::awaitable<void> {
    co_await asio::steady_timer(co_await asio::this_coro::executor, delay)
        .async_wait(asio::use_awaitable);
}

inline auto async_delay(std::shared_ptr<async_delay_params> params)
    -> asio::awaitable<std::optional<retry::detail::status_t>> {
    auto delay = retry::next_delay(params->policy, params->status);

    if(!delay) co_return std::nullopt;

    auto next = retry::detail::advance(params->status, delay.value());

    co_await params->delay_function(next.previous_delay.value_or(retry::duration_t{0}));

    co_return next;
}
}  // namespace retry

class async_delay_t {
    std::shared_ptr<retry::async_delay_params> params_;

public:
    async_delay_t() {
        this->params_ = std::make_shared<retry::async_delay_params>(retry::async_delay_params{
            .policy = retry::policy::constant{},
            .status = {},
            .delay_function = [](retry::duration_t delay) -> asio::awaitable<void> {
                co_await retry::default_async_delay_function(delay);
            },
        });
    };

    explicit async_delay_t(retry::policy_t policy) {
        this->params_ = std::make_shared<retry::async_delay_params>(retry::async_delay_params{
            .policy = std::move(policy),
            .status = {},
            .delay_function = [](retry::duration_t delay) -> asio::awaitable<void> {
                co_await retry::default_async_delay_function(delay);
            },
        });
    }

    explicit async_delay_t(retry::async_delay_function_t delay_function) {
        this->params_ = std::make_shared<retry::async_delay_params>(retry::async_delay_params{
            .policy = retry::policy::constant{},
            .status = {},
            .delay_function = std::move(delay_function),
        });
    }

    async_delay_t(retry::policy_t policy, retry::async_delay_function_t delay_function) {
        this->params_ = std::make_shared<retry::async_delay_params>(retry::async_delay_params{
            .policy = std::move(policy),
            .status = {},
            .delay_function = std::move(delay_function),
        });
    }

    auto operator()() -> asio::awaitable<bool> {
        auto status = co_await retry::async_delay(this->params_);

        if(!status) co_return false;

        this->params_->status = status.value();

        co_return true;
    }

    auto reset() -> void { this->params_->status = {}; }
};
}  // namespace crn
