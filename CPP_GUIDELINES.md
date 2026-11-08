## C++ style

### Philosophy

> If the code starts to feel "clever", it's probably wrong.
> Prefer boring, explicit, predictable code.

**A computation is a value.** `make_retry` builds a description and returns it;
calling the description runs it. Building never retries, and running never
changes the description, so the same one can be invoked twice and mean the same
thing both times. The combinators follow from that: `with_policy` returns a new
description rather than mutating the one you have.

**Pure core, effects at the edge.** `next_delay`, `apply_transformers`,
`exponential_backoff` and `advance` are pure functions of a policy and a status.
They never sleep and never read the clock. The two places that do — `delay` and
`async_delay` — do nothing else, which is why the same policy produces the same
sequence under a blocking loop and under a coroutine.

### Standard: C++23

The library needs it, and uses it deliberately:

- **Concepts** for the constraints on a retried action, never SFINAE.
  `explicitly_boolean` says what a result must be for the default check to read
  it as success; prose in a comment would not have been checkable.
- **`std::expected`** at the boundary. A retry that gives up returns a
  `retry::error_t` carrying the last exception, how many attempts were made and
  how long was spent waiting. Nothing is swallowed.
- **`std::ranges::fold_left`** and the monadic operations on `std::optional`
  for the transformer pipeline, where a delay either survives every transformer
  or the sequence ends.
- **Deducing this** for the `with_` family, so a chain moves through its own
  temporaries instead of copying at each step.
- `if constexpr` and `constexpr` defaults everywhere they fit.

Note that `std::move_only_function` would suit `check` and `delay` better than
`std::function` does, and `std::stacktrace` would enrich `retry::error_t`. Neither
is in libc++ 19, so neither is used.

### Prefer functional, avoid OOP

- Free functions over methods when state ownership is not the point. The jitter
  draw, the exponential growth and the ring wrap are free functions in `detail`,
  not members of the policies that use them.
- Plain aggregates over class hierarchies. Policies and transformers are flat
  structs with no base class, so every field can be set by name and no policy
  claims to be substitutable for another.
- No virtual dispatch. Policies and transformers are `std::variant`, and the
  dispatch is `std::visit` over an `overloaded`. A new policy that someone
  forgets to handle is a compile error, which is how the asynchronous half came
  to silently ignore a policy before the visit replaced its `if` chain.
- Inheritance only for RAII or CRTP; never to share fields and never for runtime
  polymorphism.

### Function style

Always `auto` with a trailing return type:

```cpp
auto next_delay(const policy_t& policy, const status_t& prev) -> std::optional<duration_t>;
auto make_retry(policy_t policy, F func) -> retry_t<F, T>;
```

The name comes first, which matters most where the return type is the longest
part of the signature.

### Coroutines

The asynchronous half mirrors the blocking one rather than inventing a second
vocabulary: `make_async_retry` to `make_retry`, `async_delay_t` to `delay_t`,
`async_delay` to `delay`. Both go through the same `next_delay`, and a test
asserts that a policy driven by one produces the same delays as the other. If
the two ever disagree, that is a bug in whichever one stopped sharing the core.

**A coroutine takes what it needs by value.** `async_retry_t::operator()` is not
itself a coroutine: it hands the policy, the parameters and the action to a
static one, by value. Coroutine parameters are copied into the frame, so the
running retry owns its state and cannot be left pointing at a description that
has already died — which is exactly what would happen to a captured `this` if
someone `co_await`ed a temporary.

The asynchronous loop is a loop, not the recursion its blocking twin uses. On
the stack a recursive attempt costs a frame; here it would cost a coroutine
frame kept alive until the innermost attempt returns, and the chain only unwinds
at the end.

### Failures worth retrying

A retry that treats every exception alike is a retry that hides bugs: a bad
argument or a parse error does not become true on the fourth attempt. The
`recover` predicate decides what is retryable, and whatever it turns down is
**rethrown**, not recorded — it never was a retry outcome.

Keep the kinds of failure apart. One predicate per channel, with the same
reach on each: `transient` judges a failed result, `recover` judges a thrown
exception, and both select by type or by predicate. Code that never throws must
not be the second-class citizen here. A failure called
permanent ends the sequence and the value goes back to the caller, because the
retry did reach an answer. A policy that runs out is the other ending, and only
that one produces a `retry::error_t`. An exception nobody recognised is neither:
it is a bug or a cancellation, and belongs on the exception path.

Measure before claiming a defect here. The asynchronous default refuses to
recover from `operation_aborted`, but Asio's native cancellation never reaches
that predicate: it unwinds the coroutine chain on its own. The default earns
its place for code that turns an abort into a thrown `system_error`, and the
test that covers it says exactly that, because an earlier version of this note
claimed the wrong thing and a mutation test caught it.

### Naming

`snake_case` is the default for everything that is not a type or a constant.
No `camelCase`, anywhere, including local variables and struct members.

The trailing underscore on a private member is the one exception, and it earns
its place: it is what lets `policy_` and a `policy` parameter coexist in a
constructor without one of them being renamed to something worse.

| Category               | Convention                       |
|------------------------|----------------------------------|
| Variables and members  | `snake_case` (`previous_delay`)  |
| Private class members  | `snake_case_` (`delay_function_`)|
| Types and aliases      | `snake_case_t` (`duration_t`)    |
| Concepts               | `snake_case` (`explicitly_boolean`) |
| Free functions         | `snake_case`                     |
| Compile-time constants | `SCREAMING_SNAKE_CASE`           |
| Namespaces             | short, domain-indicating         |

Domain vocabulary comes from Haskell's `retry` package and is fixed by
`README.md`. A transformer is never called a modifier, a policy is never called
a strategy, and `curr_iteration`, `cumulative_delay` and `previous_delay` keep
the names their Haskell counterparts have. If a name here disagrees with the
reference listed in the README, one of the two is a bug.

The policy and transformer types are the standing exception to the `_t` rule:
`policy::constant` and `transformer::cap_delay` carry that vocabulary and read
better unsuffixed under their namespaces. `retry::error_t` and `retry::result_t` are
not an exception, they are an oversight, and should gain their `_t`.

### Units and strong types

Durations are `std::chrono` types, never bare integers. Every delay in the
library is a `std::chrono::milliseconds`, so a call site cannot pass seconds by
accident, and the four transformers split into a counted one and three timed
ones rather than sharing an integer field that meant two different things.

Arithmetic on delays saturates rather than overflowing: `exponential_backoff`
clamps its exponent and its product, because a backoff that wraps to a negative
delay is worse than one that stops growing.

### Randomness

Anything that randomizes must be seedable from the call site, so a test can fix
the seed and get the same result every run. Every jittered policy carries a
`seeder_t`, and the tests that assert exact delay sequences supply their own.

The default seeder mixes `std::random_device` with the clock. The clock alone is
not enough: two retries in the same tick would be seeded identically and draw the
same "random" delay, which is precisely what jitter exists to prevent.

The known weakness is that `jittered` builds and seeds a generator per call
rather than being handed one that lives as long as the policy. It is correct and
reproducible, but it is more work than it needs to be, and owning a generator
would be the better shape.

### Comments

Write no comments by default. Add one only when the **why** is non-obvious: a
hidden constraint, a subtle invariant, a lesson learned from a bug.
Do not describe what the code does — well-named identifiers already do that.

**Comments go above a declaration, never inside a body.** Code that needs a
comment halfway through is usually code that wants splitting: that is how
`ringed_iteration` came to be its own named function.

```cpp
// The exponent restarts every restart_after attempts, so the delay
// keeps growing without ever running away.
auto ringed_iteration(int iteration, int restart_after) -> int;
```

A comment that restates its signature is worse than none — it is one more thing
to keep true.

---

## Build system

Meson is the only supported build system. The library itself is header-only, so
a consumer needs nothing built at all.

```sh
meson setup build
meson test -C build
```

`werror=true` at `-Wall -Wextra -Wpedantic`. The build is at zero warnings and
stays there. Consumers who hit a warning from a newer compiler can set
`-Dwerror=false` rather than wait for a fix.

Asio is optional and gates the coroutine header alone; `-Dasio=disabled` must
keep building and keep its tests passing. The blocking half depends on nothing
beyond the standard library, and that is worth protecting.

### Formatting

`.clang-format` is authoritative. Run `clang-format -i` over
`include/retry/*.hpp` and `t/*.cpp` before sending anything; the result is
stable under a second run.

There is no `.clang-tidy` and no clang-tidy target. Until there is, do not leave
`NOLINT` comments behind: they suppress checks nobody runs.

### Tests

Two kinds, side by side in `t/`, both built into one binary:

- `<name>.test.cpp` — examples. One behaviour per test, named as a sentence.
  Every bug fixed gets one here that fails before the fix; a fix without that
  test is not finished.
- `<name>.property.cpp` — invariants over generated input, with a fixed seed so
  a failure reports the same counterexample next run. Properties state what must
  hold for *any* policy: that transformer order does not matter, that a backoff
  never shrinks, that a jittered delay stays inside its bound.

Tests never sleep. A test supplies its own `delay` function, which records the
duration instead of waiting, so the whole suite runs in milliseconds and asserts
exact delay sequences. A test that calls `sleep_for` is a bug in the test.

The headers live in `include/` and the tests in `t/`, rather than side by side:
`include/` is what gets installed, and test sources have no business there.

A property is not a substitute for an example, and an example is not a
substitute for a property. The order-independence property generalises a
regression test that was written first because a real bug motivated it; both
stay.

### References

- `README.md` — what the library does, and the Haskell lineage its vocabulary
  comes from.
