# Bucket list

Possible someday, not planned. Each entry says what it would take.

## USB-C analog headset, and FM radio (2026-10-04; top of the list)

The phone has no 3.5 mm jack: stock routes the PM8916 codec's headset lines
to USB-C through an **FSA4480** analog switch (I²C `0x7af5000` @ 0x42, the
WUSB3801's bus) in Type-C *audio adapter accessory* mode, with two "USB-C
analog enable" GPIOs. The FM receiver (WCNSS iris, `APPS_FM` SMD channel,
internal FM port into the DSP) almost certainly uses that headset cable as
its antenna.

- **Adapter:** only a **passive** USB-C to 3.5 mm adapter can work (no DAC;
  DAC adapters such as Apple's or Google's are USB sound cards that need host
  VBUS, which this phone cannot supply). They have nearly vanished, so build
  one: a USB-C male breakout and a 3.5 mm TRRS socket. D+ = right, D- =
  left, SBU1/SBU2 = mic/ground (the FSA4480 swaps them), and **both CC pins
  to ground through ~1 kΩ (Ra)** to announce an audio accessory. Check the
  pinout and resistor range against the USB-C spec before wiring.
- **Kernel:** `CONFIG_TYPEC_MUX_FSA4480`; the FSA4480 node linked to the
  WUSB3801 port (mainline's `wusb3801` already reports
  `TYPEC_ACCESSORY_AUDIO`); the enable GPIOs; PM8916 MBHC detection and
  buttons; a headphone mixer path.
- **FM radio:** no mainline driver for the iris FM receiver that we know of:
  a clean-room driver for its SMD control channel, plus routing the internal
  FM audio through the DSP. Needs the headset cable as the antenna.

## Waydroid (2026-10-04)

Android apps in a container. Even stock Android was not a great experience
on this hardware, so this is a novelty, not a goal.

- Kernel: binder is built in (`binder,hwbinder,vndbinder`), containers and
  compat are there; `CONFIG_ANDROID_BINDERFS` is the missing line.
- GPU: Adreno 308 with the msm DRM driver and Mesa's freedreno, so hardware
  GLES rather than software rendering.
- Needs first: a Wayland compositor, the touchscreen, keypad/T9 input.
- Limits: 1.8 GiB RAM plus ~0.9 GiB zram (an idle Android 11 container takes
  roughly 600-900 MB), four A53 cores capped at 1.2 GHz, a 480x640 screen.
  Expect one light app at a time. Images would live on the SD card.

## Home sentinel (2026-10-04)

A power-outage and intrusion notifier that keeps working when the house
does not: days of battery, and SMS over cellular (no home power, router or
data allowance needed).

- Power loss: the charger's `online` (pm8916-lbc-chgr) drops to 0.
- Internet down: ping the router and the outside; optionally an LTE hotspot
  as a fallback (prepaid data: emergencies only).
- Tamper and intrusion from s22-sensord: accelerometer (moved), light (lights
  out, torches), barometer (door-opening pressure blips; untested), mic level.
- Needs: the SIM/modem work (SMS through QMI WMS or ModemManager), then a
  small daemon with a config of who to text and what counts as an alarm.
