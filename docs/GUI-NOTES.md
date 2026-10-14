# GUI notes

Requirements collected for when BENCtix-CAT gets a compositor and GUI.
Nothing here is built yet.

## Keys (2026-10-04)

- **◁ Back, ○ Home, □ Recents stay Android-style navigation** in the GUI
  (`KEY_BACK`, `KEY_HOMEPAGE`, `KEY_APPSELECT`). On the text console
  s22-t9d maps them to Esc, Tab and one-shot Ctrl; in the GUI it must pass
  them through unchanged. s22-t9d can tell the two apart: the console in
  front is `KD_TEXT` or `KD_GRAPHICS` (it already checks this before drawing
  its indicator). The check must not depend on the indicator being on (`-q`).
- **Speaker key** (`KEY_MICMUTE` in the device tree, from stock's key map;
  stock used it for speakerphone): speakerphone toggle during calls. Which
  physical key sends it has not been confirmed by pressing it.
- **Call / End:** answer and hang up. End's long press powers off (elogind).
- **Side key** (`KEY_NUMERIC_B`, "programmable" on stock): free to assign.
  `KEY_NUMERIC_A` (matrix r4c4) is unidentified, maybe not fitted.

## Text input

- **Predictive T9** belongs here, not on the console: it needs a candidate
  list on screen.
- The GUI needs its own **mode/candidate indicator**, like s22-t9d's console
  one: the mode (abc/Abc/ABC/123), the key's candidates with the current one
  highlighted, and at password fields a count of characters, never the
  characters themselves.

## Screens

- **Outer screen:** status (clock, battery, signal) while the lid is closed;
  builds on `s22-outer` and `s22-lidd`.
- **Main screen:** 480x640, 3"; small enough that layouts must be designed for
  it (Waydroid-style Android apps would be cramped, see the bucket list).

## Bluetooth and WiFi (2026-10-05)

The radio is shared (D019). BlueZ's background reconnect scan is set gentle
(11.25 ms every 1.28 s) so WiFi keeps the air; discovery for pairing keeps
its fast defaults. A Bluetooth settings screen may raise the reconnect scan
briefly (after resume, or while the user waits for a device to come back),
then return to the gentle setting once devices are connected.
