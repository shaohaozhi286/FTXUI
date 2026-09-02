# ACECode Local Patches to FTXUI

This file enumerates all local modifications applied to the bundled
`external/ftxui` submodule. The fork currently contains an auditable merge of
official FTXUI `main` through `989917eba88b7b67bf2b7e40fbdbe5bba66c23d5`.
When updating the submodule, merge the new official `main`, re-apply each patch,
and update this file.

Search the source for `ACECODE-PATCH(...)` comment markers to locate each
change in-tree.

## kitty-keyboard

**Goal:** let an application opt into Kitty keyboard protocol disambiguation
without changing other FTXUI consumers. ACECode uses this to distinguish
Escape and Ctrl/Alt shortcuts from legacy C0 and ESC-prefix collisions.

**Files touched:**

- `include/ftxui/component/app.hpp`
  - Adds the default-off `App::EnableKittyKeyboard(bool)` option.
- `src/ftxui/component/app.cpp`
  - `Install()` sends `CSI > 1 u` after entering the selected main/alternate
    screen and registers `CSI < u` on the LIFO cleanup stack, so pop occurs
    before leaving the alternate screen and also covers restored-I/O cycles.
  - Windows and Emscripten parser timeouts use milliseconds consistently with
    `TerminalInputParser::Timeout()` and the POSIX path.
- `src/ftxui/component/app_test.cpp`
  - Verifies default-off behavior, primary/alternate screen ordering, and
    balanced restored-I/O push/pop output.
- `src/ftxui/component/terminal_input_parser_test.cpp`
  - Verifies a Kitty `CSI u` sequence is retained as one `Event::Special` when
    supplied byte by byte.

**Risk on rebase:** if upstream changes `App::Install()` cleanup ordering or
the terminal event timeout unit, keep the keyboard push after screen entry,
pop before screen exit, and the parser threshold in milliseconds.

## drag-autoscroll

**Goal:** allow external code (the auto-scroll-on-drag handler in `main.cpp`)
to compensate FTXUI's screen-coordinate selection when the viewport scrolls
underneath a live drag, so that the highlighted region tracks the text rather
than the screen.

**Files touched:**

- `include/ftxui/component/app.hpp`
  - In `class App` public section after `SelectionChange(...)`:
    - `void ShiftSelection(int dx, int dy);`
    - `bool HasPendingSelection() const;`

- `src/ftxui/component/app.cpp`
  - After `App::SelectionChange(...)`:
    - `App::ShiftSelection(int dx, int dy)` — adds dx/dy to all four
      coordinates of `selection_data_`, resets `selection_data_previous_` so
      the next `RunOnce()` triggers a `SelectionChange` callback and
      `RefreshSelection()` re-resolves, sets `frame_valid_ = false` to force
      a redraw.
    - `App::HasPendingSelection()` — returns `bool(selection_pending_)`.

**Risk on rebase:** if upstream renames `selection_data_`, `selection_pending_`
or alters `SelectionData` field names, the patch needs to be re-applied by
hand. Both methods are 8 lines total — trivial to re-port.

## mouse-origin

**Goal:** avoid stale TerminalOutput mouse-coordinate origins during startup.
FTXUI translates mouse coordinates by subtracting the frame origin reported by
cursor-position DSR. If mouse tracking is enabled before the first post-draw DSR
is handled, early drag-selection events can use an old origin and render the
selection highlight several rows above the pointer.

**Files touched:**

- `include/ftxui/component/app.hpp`
  - Adds private `EnableMouseTracking(bool flush)` and two booleans tracking
    whether mouse tracking is enabled or waiting for cursor-position calibration.

- `src/ftxui/component/app.cpp`
  - Moves mouse tracking DECSET emission into `EnableMouseTracking`.
  - In `Install()`, TerminalOutput mode defers mouse tracking until the first
    cursor-position event after at least one frame has been drawn.
  - In `HandleTask()`, cursor-position events are accepted only when the
    reported origin plus the current frame dimensions fits inside the terminal
    viewport. Unusable startup reports are rejected, another DSR is requested,
    and mouse translation falls back to `(1,1)` instead of subtracting a stale
    bottom-of-screen origin.
  - In `Draw()`, TerminalOutput requests DSR on resize and while the cursor
    origin is deferred or invalid, including the Microsoft-terminal fallback
    path.

**Risk on rebase:** if upstream changes mouse tracking setup or cursor-position
handling in `App::Install()` / `App::HandleTask()`, re-check that TerminalOutput
does not receive mouse events before `cursor_x_` / `cursor_y_` are calibrated.

## input-trace

**Goal:** diagnose TerminalOutput mouse selection offset by logging the exact
coordinates flowing through FTXUI. This is compiled only when
`ACECODE_TUI_INPUT_TRACE` is defined and writes to `acecode.log` in the process
working directory.

**Files touched:**

- `CMakeLists.txt`
  - Adds the `ACECODE_TUI_INPUT_TRACE` option and passes the compile definition
    to the `component` library.

- `src/ftxui/component/app.cpp`
  - Logs cursor-position calibration, mouse tracking enablement, raw and
    adjusted left-drag mouse coordinates, component handling, selection state
    transitions, `ShiftSelection(...)`, and DSR requests.

**Risk on rebase:** low. The trace hooks are inside the ACECode patch areas and
can be removed or left disabled without changing normal FTXUI behavior.

## idle-mouse-redraw

**Goal:** avoid redraw churn on older Windows terminals when the pointer merely
moves over the TUI with no mouse button pressed. FTXUI's default any-event
tracking (`DECSET ?1003`) reports passive hover motion, which can repeatedly
invalidate frames and trigger visible shaking on legacy conhost / ConEmu
rendering paths.

**Files touched:**

- `src/ftxui/component/app.cpp`
  - In `App::EnableMouseTracking(...)`, uses `DECMode::kMouseBtnEventMouse`
    (`?1002h`) instead of `DECMode::kMouseAnyEvent` (`?1003h`).
  - In `App::Install()`, resets `?1002l` on exit instead of `?1003l`.

- `src/ftxui/component/app_test.cpp`
  - Updates the expected install/uninstall escape sequences from `1003` to
    `1002`.

**Risk on rebase:** if upstream changes mouse tracking setup, preserve the
principle that passive hover motion should not be enabled by default. Clicks,
wheel events, and button-held drags still require mouse reporting.

## conhost

**Goal:** keep the legacy Windows Console Host render path stable when terminal
capability probing or DEC private modes are unavailable.

**Files touched:**

- `src/ftxui/component/app.cpp`
  - Preserves the ACECode-specific conhost fallback around terminal output and
    cursor-position behavior.

**Risk on rebase:** re-check the fallback whenever upstream changes Windows
terminal detection or frame drawing.

## cjk-selection

**Goal:** prevent the filler cell following a double-width CJK glyph from being
treated as a separately selectable text cell.

**Files touched:**

- `src/ftxui/dom/text.cpp`
  - Keeps the continuation cell non-selectable while preserving the glyph's
    two-column layout.

**Risk on rebase:** re-check selection metadata if upstream changes wide-glyph
cell construction.

## synchronized-output

**Goal:** let an application opt into atomic frame presentation via DEC mode
2026 (`CSI ?2026h` / `CSI ?2026l`), so terminals that implement synchronized
updates render each frame atomically instead of exposing half-drawn
intermediate states (flicker). ACECode uses this to eliminate flicker during
high-frequency stream updates. Disabled by default; terminals that do not
implement the mode ignore the sequences harmlessly, but the embedding
application is expected to gate this on terminal detection.

**Files touched:**

- `include/ftxui/component/app.hpp`
  - Adds the default-off `App::EnableSynchronizedOutput(bool)` option, marked
    with `ACECODE-PATCH(synchronized-output)`.
- `src/ftxui/component/app.cpp`
  - `App::Internal` gains `synchronized_output_enabled_`.
  - `App::Internal::TerminalFlush()` brackets every non-empty output buffer
    with `CSI ?2026h` / `CSI ?2026l` in the same single write as the frame
    payload, so the bracket cannot be split mid-frame. Empty buffers are left
    alone.
  - `App::EnableSynchronizedOutput(bool)` forwards to the Internal flag.

**Risk on rebase:** if upstream renames the Internal output buffer or changes
`TerminalFlush()`, re-apply the bracket at the top of the flush body. The
single-write guarantee is what makes the bracket safe; keep it inside the same
`std::cout << buffer` call.

## hover-motion

**Goal:** let an application opt into passive hover motion reporting via
DECSET ?1003 (any-event tracking), which the `idle-mouse-redraw` patch
intentionally downgraded to ?1002 (button-event). ACECode uses this to show a
hover tooltip with the real URL while the pointer rests on a link. Disabled by
default; the embedding application is expected to gate this on terminal
detection (legacy conhost stays on ?1002).

**Files touched:**

- `include/ftxui/component/app.hpp`
  - Adds the default-off `App::EnableMouseHoverMotion(bool)` option, marked
    with `ACECODE-PATCH(hover-motion)`.
- `src/ftxui/component/app.cpp`
  - `App::Internal` gains `hover_motion_enabled_`.
  - `App::EnableMouseHoverMotion(bool)` forwards to the Internal flag.
  - `App::Internal::EnableMouseTracking()` sends `?1003h`
    (`DECMode::kMouseAnyEvent`) instead of `?1002h`
    (`DECMode::kMouseBtnEventMouse`) when hover motion is enabled. The
    existing uninstall sequence already resets `?1003l`, so no cleanup change
    is needed.
  - `App::Internal::RunOnce()` classifies no-button `Mouse::Moved` events as
    passive when hover motion is enabled; they are still dispatched to the
    component but do not force `frame_valid_ = false` — the component drives
    redraws explicitly via `RequestAnimationFrame()`.
- `src/ftxui/component/app_test.cpp`
  - `MouseHoverMotionDisabledByDefault`: install emits `?1002h`, never
    `?1003h`.
  - `MouseHoverMotionEnabledSendsAnyEventTracking`: install emits `?1003h`
    (and no `?1002h`), uninstall emits `?1003l`.

**Risk on rebase:** if upstream changes `EnableMouseTracking()` or the
install/uninstall escape sequence set, preserve the default-off ?1002 behavior
and the "passive motion does not invalidate frames" principle. The uninstall
path already resets `?1003l`; keep that.
