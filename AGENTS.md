# waynav

## Architecture

The grid model and config parser MUST remain independent of Wayland state and
calls. `overlay.c` owns Wayland state; `input.c` is the boundary between parsed
commands, region mutations, and overlay actions.

### Config parser

- A binding whose first command is `start` MUST store the remaining chain in
  `config.start_commands`, not as a normal binding. `main.c` executes that chain
  once after overlay creation.
- Bindings use physical xkb keycodes. The final keysequence token is a keysym
  name used only to resolve that keycode; modifier tokens determine the required
  state. For example, `h` and `H` may be aliases, while `shift+h` requires the
  physical `h` key with Shift depressed.
- All command-keyword parsing MUST use `match_keyword()` and its returned
  argument pointer. Raw prefix checks and hard-coded argument offsets
  reintroduce prefix-collision bugs.

### Overlay

The layer output and virtual pointer output hint MUST remain unset. Virtual
warps MUST translate surface-local coordinates into the union of all outputs'
logical geometries: river ignores output hints, while sway honors them. The
normal input region MUST be empty; pointer capture MUST restore it and treat
the result as one-shot. Nudge the pointer with zero relative motion if changing
the input region does not produce an enter event. Virtual warps MUST update the
cached pointer position in surface coordinates.

Losing the last keyboard MUST stop the overlay. The optional idle timer MUST
rearm on keyboard events, but not on synthetic key repeat, and release an active
drag before exiting with status 3.

niri discards a virtual pointer's axis source until a timestamped axis request
creates its pending frame.

`xkb_mods_to_config()` MUST map depressed Shift, Ctrl, Alt, and Super state to
`MOD_*` before keycode binding lookup; otherwise modified bindings silently
fail. Key events delivered reentrantly during command execution MUST be
deferred with their resolved binding snapshot until the outer binding finishes,
and repeat selection MUST be serialized in the same event order.

### Input dispatch

A command chain containing `end` MUST release keyboard focus before executing
its clicks so the client receives the primary selection.

`execute_commands()` MUST release an active drag on `end`, and `end` MUST
terminate the remaining command chain. Layer close and overlay destruction MUST
also release an active drag before the virtual pointer is destroyed. After each
command chain, it MUST request
a redraw. Each contiguous segment between `history-back` commands MUST be one
history unit: if the segment changed the region, save its pre-segment snapshot
before the following `history-back` runs. Startup commands MUST use the same
termination and redraw behavior without recording interactive history.

## Linting and logging

`misc-include-cleaner` SHOULD remain disabled because it emits false positives
for Wayland-generated headers.

Runtime diagnostics MUST use `log_*`; direct stdio is reserved for CLI
help/version output and `log.c`. Info logs SHOULD be limited to lifecycle
events.

## Integration tests

`make int-test` SHOULD be run when Docker is available after overlay,
output-selection, or protocol changes.

Synthetic integration-test failure branches MUST call `tests:eval false`
immediately before `tests:assert-success`. The duplicated `:cleanup-process`
helper MUST retain one signature across both smoke scripts.
