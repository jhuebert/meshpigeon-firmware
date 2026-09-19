# Contributing to MeshPigeon Firmware

Thanks for helping make the dumbest possible durable radio!

## The one rule that matters

The firmware stays **dumb and durable**. It stores packets it cannot read and
persists its settings; it never transmits on its own initiative. If a proposed
change gives the firmware opinions about repeating, message content, keys, or
identity — stop; it belongs in the app. This is a review gate, not a
suggestion.

## Pull requests

1. Keep changes minimal and mapped to a stated need. No drive-by refactors.
2. Every behavior change adds or extends a test in `test/` named for the
   requirement it protects.
3. Run the checks before pushing:

   ```sh
   pio test -e native            # host unit tests
   pio run                        # all board targets must build
   ```

4. C++ style: match the existing code (4-space indent, `snake_case`
   locals/functions, `PascalCase` types, doxygen-ish `///` on public APIs).
   Keep includes minimal and ordered: own header, system, local.

## Conventional commits

`feat:`, `fix:`, `docs:`, `test:`, `chore:` — one logical change per commit.

## Reporting issues

Include board, firmware version (`GET_INFO` payload), app version, and the
command trace if you have one. For RF issues: region preset, SF/BW, and the
distance/geometry.
