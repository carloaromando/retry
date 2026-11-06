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

Invoking a retry yields `std::expected<T, crn::retry_error>`:

```cpp
struct retry_error {
    std::exception_ptr cause;          // the last exception, if the action threw
    int attempts;                      // how many were made
    std::chrono::milliseconds waited;  // how long was spent waiting
};
```

Nothing is swallowed: if the action threw, the exception is in `cause`. Being
an `expected`, the result composes with the standard monadic operations.

```cpp
auto port = crn::make_retry(read_config)()
                .transform([](const config& found) { return found.port; })
                .value_or(default_port());
```

By default an attempt succeeds when its result is *explicitly* convertible to
`bool` and converts to `true` — which covers `std::optional`, smart pointers
and `std::expected` — while plain values are always accepted: an `int` of `0`
is a result, not a failure. Pass a `check` for anything else.

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
`std::expected<T, retry_error>`. Policies, transformers and `with_` combinators
are the same ones, and produce the same delay sequence.

```cpp
#include <retry/async_retry.hpp>

using namespace std::chrono_literals;
namespace policy = crn::retry::policy;

auto fetch(const std::string& url) -> asio::awaitable<crn::retry_result<std::optional<page>>> {
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
t/*.cpp` before sending anything. [GUIDE.md](GUIDE.md) has the rest.

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
