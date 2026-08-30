# Testing

## Automated suites

The development configuration currently registers 29 tests covering policy,
runtime compatibility, temporal inputs, depth, motion, resource descriptions,
debug inertness, frame pacing, limiter behavior, and provider-independent
contracts.

The public configuration adds three release contracts:

- public `UFGU.ini` safe defaults;
- signed provider runtimes, required exports, real context creation, and
  one-frame GPU smoke evaluation;
- exact 36-file package contents with zero Markdown files.

The expected public result is 32 passing tests.

## Required checks

```powershell
cmake --preset dev
cmake --build --preset dev-release
ctest --preset dev-release

cmake --preset public
cmake --build --preset public-release
ctest --preset public-release
```

## Renderer changes

A renderer, hook, temporal-input, frame-pacing, or presentation change must also
record:

- exact Skyrim and SKSE runtime;
- GPU, driver, resolution, refresh rate, ENB or ReShade state;
- provider, quality, cadence, multiplier, and limiter settings;
- comparison with upscaling off, frame generation off, and UFGU disabled;
- full `UFGU.log` and focused video or screenshots when the issue is visual.

Automated tests do not replace physical AMD, Intel, NVIDIA, ENB, or mod-list
acceptance.
