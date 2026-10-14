# BENCtix-CAT manual

How to use the phone itself. Build and design notes are elsewhere
([README](README.md), [decisions](docs/DECISIONS.md)).

## Typing on the keypad (s22-t9d)

Text input is multi-tap: tap a key repeatedly to cycle through its
characters; the character is final when you pause (about 0.9 s) or press a
different key. A tap on the same key within that time replaces the last
character, so you see each candidate as you go.

### Letter and number keys

| Key | Taps cycle through | Hold (½ s) |
|---|---|---|
| 1 | `.` `,` `?` `!` `-` `/` `_` `:` `;` `'` `"` `1` | `1` |
| 2 | `a` `b` `c` `2` | `2` |
| 3 | `d` `e` `f` `3` | `3` |
| 4 | `g` `h` `i` `4` | `4` |
| 5 | `j` `k` `l` `5` | `5` |
| 6 | `m` `n` `o` `6` | `6` |
| 7 | `p` `q` `r` `s` `7` | `7` |
| 8 | `t` `u` `v` `8` | `8` |
| 9 | `w` `x` `y` `z` `9` | `9` |
| 0 | space, `0` | `0` |
| `*` | `*` `+` `=` `@` `#` `$` `%` `&` `\|` `~` `` ` `` `^` `\` `(` `)` `[` `]` `{` `}` `<` `>` | (no hold) |

To type two letters from the same key (like `ll`), wait for the first to
settle, then tap again.

### Modes: `#`

Each tap of `#` moves to the next mode:

| Mode | Letters | Number keys |
|---|---|---|
| `abc` | lower case | letters (hold for the digit) |
| `Abc` | the **next** letter is a capital, then back to `abc` | letters |
| `ABC` | capitals until you change mode | letters |
| `123` | – | each tap is the digit itself |

`abc` → `Abc` → `ABC` → `123` → `abc`. In `123` mode, `*` still gives the
symbols, and `#` still changes mode (type a `#` character from the `*` list).

### Other keys

| Key | Does |
|---|---|
| **C** | Backspace (hold to repeat) |
| **OK** (centre of the D-pad) | Enter |
| **Call** (green) | Enter |
| **D-pad** | arrow keys (hold to repeat) |
| **◁ Back** | Esc |
| **○ Home** | Tab (shell completion) |
| **□ Recents** | Ctrl for the next character: □ then a letter. Press □ again to cancel. **Hold:** Ctrl-C twice (quits programs that ask for it twice, like Claude Code) |
| **End** (red) | tap: nothing; **hold: power off** |
| Volume up/down, speaker key, side key | passed through unchanged (no use on the console yet) |

Ctrl examples: □ `222` → Ctrl-C (stop a program), □ `3` → Ctrl-D (log
out / end of input), □ `666` → Ctrl-O (nano: save), □ `99` → Ctrl-X
(nano: exit), □ `555` → Ctrl-L (clear the screen), □ `777` → Ctrl-R (shell
history search).

### The indicator (top-right corner)

While you type, a small box in the top-right of the screen shows:

```
 abc 6:m[n]o6        mode, then the key's characters; the highlighted one is what you get
 abc ^               ^ = Ctrl is armed for the next character
 abc pw *** 7:pq[r]s7   at a password prompt: one * per character typed
```

At a password prompt only the character being chosen is shown, and only
until it is final; everything already typed is a `*`, so you can count
them. `C` removes one, Enter starts over. The box clears itself after a
few seconds without typing.

### Settings

`/etc/conf.d/s22-t9d`, then `rc-service s22-t9d restart`:

```
T9D_ARGS="-t 900 -l 500"   # -t: ms to tap the same key again, -l: ms to hold for a digit
T9D_ARGS="-q"              # no indicator
T9D_ARGS="-b 8000"         # keypad backlight on for 8 s after the last key (default 5 s)
T9D_ARGS="-b 0"            # leave the keypad backlight alone
```

The keypad backlight comes on at the first key press and goes off a few
seconds after the last one.

`rc-service s22-t9d stop` gives the raw keypad back (digits only, no
letters); `start` brings T9 back.

## The phone

- **Lid:** closing it turns the main screen off and WiFi power save on (about
  17 mA less; off again when you open it, so ssh stays quick; stays off with
  the lid closed while a Bluetooth speaker or headphones are connected).
  The phone then sleeps while nothing needs it (Settings, Display); a
  call, a text, a key or opening the lid wake it. After 2 minutes closed
  Tailscale pauses (it woke the phone every 24 s) and comes back when you
  open the lid (Settings, VPN: keep it connected). More lid actions: scripts in
  `/etc/s22-lidd.d/`, run with `open` or `closed`.
- **Outer screen:** with the lid closed it lights for 10 seconds when you
  close the lid, press the side, volume or power key, or a notification
  arrives: time, date, WiFi, Bluetooth, battery and the newest notification
  (scrolling when it is long). Settings in `/var/lib/s22-outer/settings`
  (applied at once):
  ```
  clock=12        # or 24
  blink=yes       # the colon blinks
  timeout=10      # seconds lit
  ```
  Wallpaper: put a binary PPM (any size) at `/var/lib/s22-outer/wallpaper.ppm`,
  e.g. `magick photo.jpg /var/lib/s22-outer/wallpaper.ppm` on a PC, then copy it.
- **Lid-closed keys:** **side key** = wake, then the next notification
  (the badge shows which, e.g. 2/3); **hold the side key** = dismiss the one
  shown. **Volume keys** = media volume (a bar shows it on the outer screen);
  **hold** = next / previous track (with a player that supports MPRIS). The
  volume keys work with the lid open too.
- **Notifications:** `s22-notify "text"` posts one, `s22-notify -s sms "text"`
  posts one from a source, `s22-notify -l` lists them, `s22-notify -c` clears
  yours. Any user can post. Each one vibrates with its source's pattern, set
  in `/var/lib/s22-notify/vibrate` (applied at once):
  ```
  default=200,150,200   # on,off,on in ms
  sms=600
  alarm=none            # no vibration
  strength=100          # percent
  ```
- **End key:** tap does nothing, hold powers off.
- **lk2nd's fastboot without the key press:** `sudo reboot-lk2nd` reboots
  straight into lk2nd's fastboot (`fastboot boot ...` for test images,
  `fastboot reboot` to come back). `reboot-lk2nd -n` only arms it for the next
  reboot; `reboot-lk2nd -c` disarms it. It is one-shot: the boot after that is
  normal again.
- **Logging in:** `micro` on the keypad, or over ssh: `ssh micro@172.16.42.1`
  (USB) or `ssh micro@ADDRESS` (WiFi: the address Settings shows). `sudo` asks for micro's password.

## Text messages

Incoming texts buzz (the `sms` vibration pattern) and show on the outer
screen like any notification, from the sender's number. Every message is also
kept in `/var/lib/s22-sms/inbox`, one per line (time, number, text):
```
column -t -s $'\t' /var/lib/s22-sms/inbox | tail
```
Long texts arrive as one message once all their parts are in. Picture and
group messages (MMS) show as "not supported yet". Mobile data stays off.
Modem state, as root:
```
qmicli -d qrtr://0 --nas-get-serving-system     # registration
qmicli -d qrtr://0 --uim-get-card-status        # SIM
```

## Calls

Incoming calls buzz and show on the outer screen. Open the flip (or press
Call) to answer; close it (or press End) to hang up or reject. Calls use
the earpiece and the microphone under the keypad. From a shell:
```
s22-call dial 5551234567     # place a call
s22-call answer | hangup | status
```

## Battery

`s22-batteryd` logs the battery every 5 minutes to
`/home/s22-battery/battery-YYYY-MM-DD.csv` (on the SD card): charge, voltage,
lid, and how much the modem and the whole system slept. Below 5% while
unplugged it warns (buzz and outer screen), waits 30 s and powers off;
plugging in during those 30 s cancels it. It does this once: after such a
power-off it stays disarmed until the battery is charged back to 20%.

## GPS

The modem's receiver (GPS and GLONASS) only listens: nothing goes over the
cellular network. It runs only while a program holds it, and the status
bar shows the location arrow then (grey while it searches, white with a
fix). From the shell:

```
s22-gnss          # a line per second until Ctrl-C
s22-gnss -1       # wait for a fix (within 50 m), print it, stop
s22-gnss -s       # the last position, without turning GPS on
```

Outdoors a cold start takes about half a minute; once it has run recently,
a few seconds. Programs hold it through `/run/s22-gnss/ctl` (group wheel;
`nmea` instead of `hold` gives NMEA sentences). The position is in
`/run/s22-gnss/state`, readable by root and wheel only.

## The modem and IMS

`s22-imsd` keeps the modem's IMS data connection (the carrier's `ims` APN)
up when the modem asks for it; without it the modem never sleeps. It never
starts internet data. Log: `/var/log/s22-imsd.log`.

## Bluetooth and audio

Pairing (as root, with the device in pairing mode):
```
bluetoothctl
  scan on             # wait for it; older devices: "scan bredr"
  pair XX:XX:...      # then: trust XX:XX:...  and  connect XX:XX:...
```
Trusted devices reconnect by themselves.

Audio goes through PipeWire, which runs for your account. A connected
Bluetooth speaker or headphones become the default output:
```
wpctl status                                  # outputs, streams
wpctl set-volume @DEFAULT_AUDIO_SINK@ 50%     # or 5%+ / 5%-
mpg123 -o alsa song.mp3                        # play a file
```
Bluetooth audio stutters while WiFi is busy (they share one radio; fix
pending), so play downloaded files rather than streams for now.

For a new account, once: `rc-update -U add pipewire default`,
`rc-update -U add wireplumber default`, `rc-update -U add pipewire-pulse default`.
They start at the next login.

## Shell

zsh with oh-my-zsh (`af-magic`): a divider line before each prompt; on the
right, a failed command's exit status (`1 !` on the phone, `1 ↵` over ssh)
and `user@host`. `ll` = `ls -lah`.
