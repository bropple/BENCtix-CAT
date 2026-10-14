# The GUI: structure and roadmap

How the pieces fit (D029) and what comes in which order. GUI-NOTES.md has
older requirement notes, some written for another device.

## Layers

| Layer | What | Talks through |
|---|---|---|
| Services (root) | s22-smsd, s22-calld, s22-imsd, s22-battery, s22-notifyd | their sockets, /run state files, the SMS inbox, /run/s22-notify |
| Compositor | s22-wm: windows, keys, display power, layer-shell overlays | Wayland; control socket |
| Shell | s22-shell: notification banners and the notification list | layer-shell surfaces; /run/s22-notify |
| Toolkit | s22ui: drawing, text, lists, header/hints, settings, themes | linked into every app |
| Apps | launcher, Settings, Messages, Phone, ... (.app files) | the services' files and sockets |

Rules of thumb:

- **Services own the data**, apps show it. An app never talks QMI; it reads
  what a service writes or asks the service's socket. That keeps the
  modem's rules (ask before sending) in one place.
- **One window per app**, app_id `s22-<name>`. Opening an app that is
  running raises it (s22-wm `raise`), so apps keep their state.
- **Keys:** d-pad moves, OK chooses, Call does the call-ish thing, Back goes
  back (closes the app from its first page), Home and End go home, Recents
  to the previous app, digits are digits in our apps.
- **Everything shared is a file** with key=value lines or TSV, readable
  without a library: settings (/var/lib/s22/settings), themes
  (/usr/share/s22/themes), notifications (/run/s22-notify), the SMS inbox.

## Apps working together: links

Apps hand each other work as links, "scheme:rest" strings, the way Android
uses intents. An app's .app file lists the schemes it opens (`Handles=sms`);
`ui_open_uri()` finds that app and either hands the link to the running one
(its socket, $XDG_RUNTIME_DIR/s22-app-ID.sock, "open LINK") and raises it,
or starts it with the link as its argument. The app gets it in its open()
callback. Notifications carry a link too (`s22-notify -o LINK`), which
opening the notification follows.

| Link | Opens | Who sends it |
|---|---|---|
| `sms:NUMBER` | Messages, that conversation | s22-smsd's notifications; later Contacts, the call log |
| `tel:NUMBER` | Phone, the number ready to call | later: Messages, Contacts, the call log |
| `contact:ID` | Contacts, that card | later: Phone, Messages |
| `file:PATH` | the app for its type (Photos for images) | later: Camera; Messages for MMS pictures |

Shared data stays with the app that owns it, readable by the others:
contacts in one file under ~/.local/share/s22 with a lookup in s22ui (a
name for a number) for Phone, Messages and the call log; photos as plain
files in ~/Pictures for Camera, Photos and MMS.

## Notifications

Posting stays `s22-notify -s SOURCE TEXT` (a file in /run/s22-notify).
s22-notifyd vibrates, s22-outerd shows the newest on the outer screen, and
s22-shell shows each new one as a banner dropping in at the top of the main
screen for a few seconds. The side key opens the notification list: d-pad to
choose, OK follows its link or opens the app for its source (sms:
Messages, call: Phone) and dismisses it; one with neither is shown whole and
stays. C dismisses one, # all, Back closes. A dismissed notification is only the alert: the
message itself stays in its app.

## Themes

Colors come from the theme named by `theme=` in the settings (default
`dark`): /usr/share/s22/themes/NAME.theme, key=value with #rrggbb values
(bg, fg, dim, accent, bar, sel, ok, warn). Apps draw with the theme's
colors, never with literal ones, so a theme changes everything. Fonts and
sizes may follow later.

## Roadmap

1. **Foundations and Messages** (now): themes; layer-shell in s22-wm;
   s22-shell (banners, notification list); Messages (conversations from the
   inbox, read state).
2. **Phone:** dialer, incoming-call screen (s22-calld raises it), in-call
   screen (end, mute, speaker, volume), call log. s22-calld gets an event
   socket for the GUI.
3. **Text entry:** an s22ui multi-tap entry with its mode indicator, and
   replies and new texts in Messages (s22-smsd sends, only when asked).
   (Done ahead of Phone.) Predictive T9 after that.
4. **Contacts:** names for numbers in Messages, Phone and the call log;
   links between them (above).
5. **MMS:** pictures, group texts and long texts from some phones. The
   modem gets only a WAP push (an SMS to port 2948, which s22-smsd already
   spots); the message itself is downloaded over HTTP from the carrier's
   MMSC through a separate data connection (the MMS APN), and sending is an
   upload the same way. Needs mobile data on demand first, and it uses the
   data plan: ask before the first fetch. Then pictures in Messages, from
   Camera and Photos through file: links.
6. **System:** status icons (signal, data), power menu (End held), Wi-Fi
   networks and Bluetooth pairing in Settings, volume, alarms.
7. **Later:** touch (if the main screen has it), more themes, animations.
