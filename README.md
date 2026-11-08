# retry

Retry combinators for C++23. Header-only.

A retry is built as a value and run later. You describe *what* to do, and
separately *how long to keep trying*; the two compose, and nothing runs until
you call it.

```cpp
#include <retry/retry.hpp>

using namespace std::chrono_literals;
namespace policy = crn::retry::policy;

auto fetch = crn::make_retry(
    policy::exponential{
        .transformers = {policy::transformer::limit_attempts{5}},
        .base = 200ms,
    },
    [&] -> std::optional<page> { return http_get(url); });

if(auto result = fetch(); result)
    use(result.value().value());
else
    log("gave up after {} attempts", result.error().attempts);
```

## What it is for

Anything that fails for a while and then stops failing: a service that is still
starting, a rate-limited API, a flaky network, a lock held by someone else.
Writing that loop by hand is easy; writing it so it backs off, gives up at the
right moment, and tells you why it failed is less so.

## Using it

`make_retry` does not run anything. It returns a description, and calling that
description runs it. Running it never changes it, so the same description can
be invoked again and means the same thing.

```cpp
auto fetch   = crn::make_retry(http_get);
auto patient = fetch.with_policy(policy::constant{.delay = 2s});
auto picky   = patient.with_check([](const auto& page) { return page && !page->empty(); });
```

Each `with_` returns a new description; `fetch`, `patient` and `picky` are
three independent values.

### Results

Invoking a retry yields `crn::retry::result_t<T>`, which is
`std::expected<T, crn::retry::error_t>`:

```cpp
namespace crn::retry {
struct error_t {
    std::exception_ptr cause;          // the last exception, if the action threw
    int attempts;                      // how many were made
    std::chrono::milliseconds waited;  // how long was spent waiting
};
}
```

Nothing is swallowed: if the action threw, the exception is in `cause`. Being
an `expected`, the result composes with the standard monadic operations.

```cpp
auto port = crn::make_retry(read_config)()
                .transform([](const config& found) { return found.port; })
                .value_or(default_port());
```

#### Chaining retries

Every retry fails with the same `error_t`, so `and_then` chains them without
any adapting. Each step carries its own policy and is retried on its own; the
first one to exhaust its policy short-circuits the rest.

```cpp
auto quiet = policy::exponential{.transformers = {policy::transformer::limit_attempts{5}},
                                 .base = 50ms};

auto size = crn::make_retry(quiet, resolve)()
                .and_then([&](const endpoint_t& endpoint) {
                    return crn::make_retry(quiet, [&endpoint] { return connect(endpoint); })();
                })
                .and_then([&](const session_t& session) {
                    return crn::make_retry(quiet, [&session] { return fetch(session); })();
                })
                .transform([](const std::string& body) { return body.size(); })
                .value_or(0);
```

If `connect` never succeeds, `fetch` is never called and the chain ends with
that step's `error_t`. What it does not tell you is *which* step gave up —
`error_t` carries the count, the time waited and the last exception, not a
name. When that matters, label each step as it fails:

```cpp
struct step_error_t {
    std::string_view step;
    retry::error_t error;
};

auto at = [](std::string_view step) {
    return [step](retry::error_t error) { return step_error_t{step, std::move(error)}; };
};

auto page = crn::make_retry(quiet, resolve)()
                .transform_error(at("resolve"))
                .and_then([&](const endpoint_t& endpoint) {
                    return crn::make_retry(quiet, [&endpoint] { return connect(endpoint); })()
                        .transform_error(at("connect"));
                });
```

By default an attempt succeeds when its result is *explicitly* convertible to
`bool` and converts to `true` — which covers `std::optional`, smart pointers
and `std::expected` — while plain values are always accepted: an `int` of `0`
is a result, not a failure. Pass a `check` for anything else.

### Which failures to retry

A retry that treats every failure alike hides bugs: a 404 does not become a 200
on the fourth attempt, and a bad argument never will. Two predicates decide,
one per channel.

**A failed result** — `transient` says whether it is worth another go. The
default is yes, for everything.

```cpp
auto fetch = crn::make_retry(policy, get,
                             {.transient = [](const response_t& r) { return r.status >= 500; }});
```

When the action returns a `std::expected`, `on_error` reaches into the error
for you, whatever shape it has:

```cpp
// a struct, judged by a field
{.transient = crn::retry::on_error([](const http_error_t& e) { return e.status >= 500; })}

// an enum, judged by value
{.transient = crn::retry::on_error([](code_t code) { return code == code_t::unavailable; })}
```

An error shaped as a `std::variant` — what a codebase that avoids exceptions
usually carries — gets selection by type, the same way the throw channel does:

```cpp
{.transient = crn::retry::transient_only<timed_out_t, throttled_t>()}
{.transient = crn::retry::transient_except<not_found_t>()}
```

**A thrown exception** — `recover` says the same thing for the throw channel.
Anything it turns down is **rethrown** rather than counted, slept on and tried
again, the way Haskell's `recovering` rethrows what no handler matches.

```cpp
{.recover = crn::retry::recover_only<timeout_t, connection_lost_t>()}
```

| Channel | Predicate | Retries on |
| --- | --- | --- |
| result | *(default)* | every failed result |
| result | `on_error(p)` | errors the predicate accepts |
| result | `transient_only<Es...>()` | only these variant alternatives |
| result | `transient_except<Es...>()` | everything but these alternatives |
| throw | `recover_all` | anything (the default) |
| throw | `recover_none` | nothing — the first throw ends the sequence |
| throw | `recover_only<Es...>()` | only these types, base classes included |
| throw | `recover_except<Es...>()` | everything but these types |

Neither channel is the privileged one. Code that propagates failures as values
rather than as exceptions has the same selectivity, by type or by predicate,
without ever touching `recover`.

`recover_all` is the default because it surprises nobody, but it is the wrong
choice as soon as the action can throw for a reason trying again will not fix.
Narrow it.

#### Both styles, side by side

The same policy drives an action that returns its errors and one that throws
them. Only the predicate changes.

```cpp
namespace retry = crn::retry;

auto backoff = policy::full_jitter{
    .transformers = {policy::transformer::limit_attempts{5},
                     policy::transformer::cap_delay{2s}},
    .base = 200ms,
};

// std::expected<std::string, std::variant<throttled_t, unavailable_t, forbidden_t>>
auto page = crn::make_retry(backoff, download,
                            {.transient = retry::transient_except<forbidden_t>()})();

// std::string, throwing timed_out_t or bad_request_t
auto sent = crn::make_retry(backoff, upload,
                            {.recover = retry::recover_only<timed_out_t>()})();
```

Both retry what is worth retrying and stop at once on what is not: a
`forbidden_t` comes straight back as a value, a `bad_request_t` is rethrown.

### Two ways to end

The outer `expected` answers one question only: did the retry reach an answer?

- **It has a value** — the retry finished. Usually that is a success; it is
  also what you get when `transient` called a failure permanent, in which case
  the value is the failure the action reported. Nothing is lost.
- **It has an error** — the policy ran out before any answer arrived, and
  `retry::error_t` says how many attempts were made, how long was spent waiting
  and what the last exception was.

An action returning `std::expected<V, E>` therefore nests: you get
`std::expected<std::expected<V, E>, retry::error_t>`. The two layers mean
different things — the outer is about the retry, the inner about the action —
and flattening them would throw away the distinction.

### Policies

| Policy | Delay before attempt *n* |
| --- | --- |
| `constant` | `delay` |
| `exponential` | `base * 2^n` |
| `full_jitter` | random value in `[0, base * 2^n]` |
| `ringed_exponential_with_jitter` | like `full_jitter`, but *n* restarts every `restart_after` attempts |

The ringed variant suits long-lived workers that must keep retrying forever
without letting the delay grow without bound. Exponential growth saturates
rather than overflowing.

### Transformers

Every policy carries transformers that bound it. They compose in order: each
sees the delay the previous ones produced, and the first one to end the
sequence stops the rest.

| Transformer | Effect |
| --- | --- |
| `limit_attempts{n}` | stop after `n` attempts |
| `limit_by_delay{d}` | stop once a single delay would exceed `d` |
| `limit_by_cumulative_delay{d}` | stop once the total time waited exceeds `d` |
| `cap_delay{d}` | clamp each delay to `d` |

The default is `limit_attempts{5}`. Every delay is a
`std::chrono::milliseconds`, never a bare number, so the unit is part of the
type. Policies are flat aggregates, so you name only the fields you care about:

```cpp
policy::ringed_exponential_with_jitter{
    .transformers = {policy::transformer::cap_delay{30s}},
    .base = 500ms,
    .restart_after = 3,
};
```

### Waiting without retrying

`crn::delay_t` applies a policy's backoff on its own, for loops you write
yourself. Unlike a retry it is deliberately stateful: each call advances the
sequence, returns `false` once the policy is exhausted, and `reset()` starts
over.

```cpp
crn::delay_t backoff{policy::exponential{}};

while(!connect())
    if(!backoff())
        return fail();
```

### With Asio

`<retry/async_retry.hpp>` mirrors the blocking half for coroutines.
`make_async_retry` takes an action returning `asio::awaitable<T>` and gives back
a description you `co_await`; the result is the same
`std::expected<T, retry::error_t>`. Policies, transformers and `with_` combinators
are the same ones, and produce the same delay sequence.

```cpp
#include <retry/async_retry.hpp>

using namespace std::chrono_literals;
namespace policy = crn::retry::policy;

auto fetch(const std::string& url) -> asio::awaitable<crn::retry::result_t<std::optional<page>>> {
    auto retrying = crn::make_async_retry(
        policy::full_jitter{
            .transformers = {policy::transformer::limit_attempts{5}},
            .base = 200ms,
        },
        [&url]() -> asio::awaitable<std::optional<page>> { co_return co_await http_get(url); });

    co_return co_await retrying();
}
```

Run it the usual way:

```cpp
asio::io_context io;

asio::co_spawn(
    io, [&]() -> asio::awaitable<void> { result = co_await fetch(url); }, asio::detached);

io.run();
```

The waiting is an `asio::steady_timer` on the coroutine's own executor. Pass
your own `async_delay_function_t` to wait differently — the tests pass one that
records the duration and returns immediately, which is how the suite asserts
exact delay sequences without ever sleeping.

#### Cancellation

Asio's own cancellation unwinds the retry by itself: the loop stops where it
was cancelled rather than treating the abort as a failed attempt. Code that
turns a cancelled operation into a thrown `system_error` is the case the
asynchronous default `recover` covers — it recovers from everything **except**
`operation_aborted`, because swallowing a cancellation and trying again is
never what the caller asked for. Other Asio errors, a refused connection among
them, are retried normally.

### Backing off by hand

`crn::async_delay_t` is the coroutine twin of `delay_t`, for loops you want to
write yourself: `co_await backoff()` suspends instead of blocking, and returns
`false` once the policy is exhausted.

```cpp
crn::async_delay_t backoff{policy::exponential{}};

while(!co_await connect())
    if(!co_await backoff())
        co_return failure();
```

## Requirements

- A C++23 compiler and standard library. Developed and tested with **clang 19
  and libc++ 19**; other toolchains are untested. The library needs
  `std::expected`, `std::ranges::fold_left`, the monadic operations on
  `std::optional`, and deducing this.
- **Asio**, only for `<retry/async_retry.hpp>`. The blocking half has no
  dependencies beyond the standard library.
- Meson 1.1 and GoogleTest, only to build the tests.

## Adding it to your project

It is header-only: putting `include/` on your include path is enough.

As a Meson subproject, drop a `retry.wrap` in `subprojects/`:

```ini
[wrap-git]
url = https://github.com/carloaromando/retry.git
revision = head

[provide]
dependency_names = retry, retry-async
```

```meson
retry_dep = dependency('retry')
executable('app', 'main.cpp', dependencies : [retry_dep])
```

`dependency('retry-async')` gets you the coroutine header and pulls in Asio.
Built as a subproject the test suite is skipped.

With Nix, add the flake as an input and take `packages.default`.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `-Dasio=` | `auto` | `disabled` skips `async_retry.hpp` and its tests |
| `-Dtests=` | `true` | `false` skips the test suite |
| `-Dwerror=` | `true` | `false` stops treating warnings as errors |

```
meson setup build
meson test -C build
```

Asio and GoogleTest come from the system when present and from the Meson
wrapdb otherwise. The build is warned at `-Wall -Wextra -Wpedantic`; if a
future compiler grows a warning this code has not seen, build with
`-Dwerror=false`.

With Nix: `nix develop` for a shell, `nix build .#unittests` to build and run
the tests, `nix build .#no-asio` for the build without the async header.

## Contributing

`t/` holds `*.test.cpp` for examples and `*.property.cpp` for invariants over
generated input. Tests never sleep: they pass their own `delay` function, which
records the duration instead of waiting, so the suite runs in milliseconds and
can assert exact delay sequences.

`.clang-format` is authoritative; run `clang-format -i include/retry/*.hpp
t/*.cpp` before sending anything. [CPP_GUIDELINES.md](CPP_GUIDELINES.md) has the rest.

## References

The design is a port of Haskell's `retry`, by way of its TypeScript
descendant. The lineage is visible in the names: `retry_t`'s internal status
carries `curr_iteration`, `cumulative_delay` and `previous_delay`, which are
Haskell's `rsIterNumber`, `rsCumulativeDelay` and `rsPreviousDelay`, and the
policies and transformers are `constantDelay`, `exponentialBackoff`,
`fullJitterBackoff`, `limitRetries`, `capDelay`, `limitRetriesByDelay` and
`limitRetriesByCumulativeDelay` under C++ names.

- [`retry`](https://hackage.haskell.org/package/retry) — the Haskell package
  this follows, and the origin of the policy and transformer vocabulary.
- [`retry-ts`](https://github.com/gcanti/retry-ts) — Giulio Canti's TypeScript
  port, by way of [`purescript-aff-retry`](https://github.com/Unisay/purescript-aff-retry).
  Its `RetryStatus` is the shape used here.
- Ivan Čukić, *Functional Programming in C++* (Manning, 2018) — for treating a
  computation as a value you build and pass around rather than an action you
  perform.
- [Exponential Backoff And Jitter](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)
  — the AWS article that defines the full-jitter strategy.

## License

BSD-3-Clause. See [LICENSE](LICENSE).
