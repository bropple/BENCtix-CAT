# BENCtix-CAT decisions

Numbered, newest last. Each says what was decided and why; superseded
entries stay, marked.

## D001 Storage layout (2026-10-04)

- `/`: eMMC `userdata` (11.4 GiB), **btrfs with zstd compression**.
  Compression stretches the small eMMC; snapshots give a rollback before
  updates.
- `/boot`: eMMC `cache` (256 MiB), **ext2**. See D002.
- `/home`: the microSD card (118 GiB), **f2fs**, mounted `nofail` so a
  missing card never stops boot.
- pacman's package cache on the SD card; swap is zram, not on the card.
- `super` (2.5 GiB, stock Android images) is left alone for now: it still
  holds stock vendor files some tools are extracted from.

## D002 Boot chain: lk2nd reads extlinux from an ext2 /boot (2026-10-04)

lk2nd (in the `boot` partition) mounts every partition of 16 MiB or more as
ext2 and boots the first `/extlinux/extlinux.conf` it finds. Its ext2 driver
has no extent support and it cannot read btrfs, so `/boot` is a separate
plain ext2 filesystem on `cache`, which Android no longer needs.

lk2nd supports gzip kernels, `fdt`/`fdtdir`, `initrd`, `append` and several
labels with a `default`, so kernels are installed as files on `/boot` and
updates never write to the `boot` partition. Paths in `extlinux.conf` are
relative to the partition root (no `/boot` prefix).

## D003 Partition protection (2026-10-04)

Linux must not be able to damage the boot chain or stock partitions.

1. As the earliest boot step, every eMMC partition is set read-only
   (`BLKROSET`) except `userdata` and `cache` (D007; originally also the
   EFS partitions rmtfs writes). This catches mistakes and
   buggy tools; root could still undo it deliberately.
2. If the eMMC supports it, **power-on write protection** over the
   bootloader region (stock bootloaders, `boot` with lk2nd, `dtbo`). Even
   root cannot clear it before the next reset, and a reset clears it, so
   reflashing from lk2nd's fastboot still works.

## D004 Kernel naming and rollback (2026-10-04)

- Package `linux-benckernel-cat`, release `<kernel>-<pkgrel>-BENCkernel-CAT`
  (e.g. `7.1.3-1-BENCkernel-CAT`): never confusable with ALARM's
  `linux-aarch64`, as in BENCtix (its D190).
- Each build has its own module directory. On upgrade a pacman hook keeps
  the outgoing kernel, DTB, initramfs and modules, and `extlinux.conf` gets
  a `current` and a `previous` label.

## D005 btrfs built in (2026-10-04)

`CONFIG_BTRFS_FS=y` so the root filesystem mounts even if the initramfs is
broken. Distro-level config choices live in `config/benckernel-cat.config`;
hardware support stays in the kernel fork's `s22flip.config`.

## D006 pacman-sd (2026-10-04; built 2026-10-08, see D046)

Optional packages on the SD card through `pacman-sd`: pacman with its root,
database and cache on the card; host packages seeded into its database and
ignored, so nothing is duplicated or touched; apps launched by `sd-run`
with the card's `/usr` overlaid on the host's through bubblewrap; a hook
upgrades the card after each `pacman -Syu`. No system-wide `/usr` overlay.
Built once the base system boots.

## D007 The modem EFS lives in files; only /boot and / are writable (2026-10-04)

The eMMC write-protect granularity is 4 MiB (HC_WP_GRP_SIZE 8 x 512 KiB). The
small EFS partitions rmtfs writes (`fsc`, `fsg`, `modemst1`, `modemst2`)
share 4 MiB groups with `sbl1`, `rpm`, `tz`, `cmnlib`, `sec` and `DDR`, so
while they are written, the primary bootloaders cannot be hardware-protected.

So rmtfs serves the EFS from files in `/var/lib/rmtfs` on `userdata`
(`rmtfs -o`), seeded once from the partitions. The EFS partitions are never
written again: the IMEI and calibration stay pristine on the eMMC (and in
the partition backup), and a wiped `userdata` is re-seeded from them.
Supersedes the EFS part of D003's allowlist: only `userdata` and `cache`
are writable, and every other eMMC byte (except the backup GPT, which
shares the last group with `userdata`) can get power-on write protection.

Measured on the phone (2026-10-04): with D007, `s22-partguard` protects
blocks 0-483327 (GPT through `metadata`, including `sbl1`, `rpm`, `tz`, the
EFS partitions and `persist`) and 1015808-6897663 (`logdump` through `super`,
including `aboot`, `dtbo`, `vbmeta`, `boot` with lk2nd). Writes there fail
with I/O errors at the eMMC even through `/dev/mmcblk0`, and a normal
reboot clears the protection.

## D008 No initramfs (2026-10-04)

ALARM's `mkinitcpio` depends on systemd, which BENCtix-CAT removes (BENCtix
drops it for the same reason). Everything the root filesystem needs is built
into BENCkernel-CAT: the eMMC (`sdhci-msm`, `mmc_block`), its clocks,
regulators, pinctrl and interconnects (`gcc-msm8917`, `smd-rpm`, SPMI),
GPT and btrfs (D005). The kernel finds the root itself with
`root=PARTLABEL=userdata rootwait`, so the package no longer depends on
mkinitcpio. `benckernel-cat-update` still builds an initramfs and adds an
`initrd` line if mkinitcpio is ever installed.

## D009 Rootless build in user namespaces (2026-10-04)

The rootfs stages run as the normal user. Each re-executes itself under
`unshare --map-auto --map-root-user --mount --pid` (subordinate uids from
`/etc/subuid`), where it is root over the rootfs and nothing else. Mounts
(the chroot's /proc, /dev, the package cache) exist only in that namespace
and vanish when it exits, so a deleted rootfs can never take a bind-mounted
cache with it (BENCtix C038, D074), and no step needs sudo. A side effect
seen in the first run: ALARM's tmpfiles hook tried to chown the host's /dev
nodes through the bind and was refused, where real root would have
succeeded.

## D010 The OpenRC overlay comes from BENCtix (2026-10-04)

BENCtix has already built the aarch64 OpenRC stack from Artix's PKGBUILDs
(openrc, udev, elogind, pam, dbus, util-linux, procps-ng, pacman and the
D364 rebuilds). Rebuilding it under emulation takes hours and would give the
same packages, so stage 20 copies the package files it needs from BENCtix's
`work/repo` (read-only) and records their sha256 in
`snapshot/overlay.sha256`. The arch=any init scripts and `base` come from
Artix's mirror, as in BENCtix. Only what ALARM lacks is built here: `qrtr`,
`rmtfs` and `s22-sensord` (`packaging/`).

## D011 Install by flashing two images from lk2nd (2026-10-04)

Stage 50 builds `boot.img` (ext2, the cache partition: kernel, DTB,
extlinux.conf) and `root.simg` (btrfs with zstd:3, the userdata partition,
as an Android sparse image). `tools/flash-benctix-cat.sh` flashes exactly
those two from lk2nd's fastboot and nothing else. The root filesystem has
subvolumes `@` (the root, set as default) and `@snapshots`, for snapshots
before updates. The images fill the partitions exactly (sizes read from the
phone), so nothing has to grow on first boot.

## D012 Reaching the phone: USB network and ssh keys (2026-10-04)

`s22-usbnet` brings up an NCM gadget with fixed, locally administered MAC
addresses; the phone is 172.16.42.1 and dnsmasq gives the host 172.16.42.2
(DHCP only, no route or DNS). root logs in over ssh with the build host's
key only; its password is locked and the stock `alarm` account is removed.
No serial getty on a USB ACM port: the host's ModemManager probes ACM ports
and would type AT commands into it. If a build does not come up, lk2nd's
fastboot can still boot the RAM bring-up image, since nothing here writes
the boot partition.

## D013 ALARM packages that still declare systemd (2026-10-04)

Three ALARM packages in the image declare systemd dependencies after the
swap (`pacman -Dk`): `btrfs-progs` (systemd-libs, for libudev, which ours
provides), and `pcsclite` and `polkit`, which wpa_supplicant pulls in. Their
daemons `pcscd` and `polkitd` link libsystemd and are allowlisted in stage
20's scan: no init script starts them, wpa_supplicant uses only the clean
`libpcsclite`, and elogind uses polkit only optionally. All three are in
IgnorePkg, because upgrading them on the phone would pull systemd in (the
refuse hook would abort the whole transaction). To do: Artix rebuilds of all
three, as BENCtix did for its D364 set.

## D014 lk2nd stays (2026-10-04)

Booting BENCkernel-CAT straight from the stock aboot (an Android boot image
in `boot`) was considered and rejected; the user wants to stay away from
partition-level changes. With lk2nd, kernel updates only write files on
/boot, `boot` keeps its power-on write protection, `current`/`previous`
gives a fallback kernel, mainline device trees work without aboot's msm-id
and board-id selection, and lk2nd's fastboot can boot the RAM bring-up image
for recovery. The 7.1.3-2 -> 7.1.3-3 upgrade on the phone exercised exactly
this path. lk2nd changes only if the boot flow itself does.

## D015 Clock: RTC plus a saved offset, and chrony (2026-10-04)

The PM8916 RTC keeps counting while the phone is off, but Linux cannot set
it (rtc-pm8xxx has no `allow-set-time` here; Android kept an offset in a
daemon). `s22-clock` (boot runlevel, replaces `hwclock`) sets the time from
RTC + `/var/lib/s22-clock/offset` at every boot, so the time is right even
with no network. It saves the offset once chrony reports a sync, and again
at a clean shutdown if synced. chrony (ALARM, with Artix's `chrony` init
script) keeps the time over WiFi; `makestep 1.0 -1` lets it step from a cold
1969 clock, and there is no `rtcsync`. WiFi is `wpa_supplicant` on `wlan0`
with dhcpcd; network credentials are added on the phone only, never in git.

Update 2026-10-08: the post-sync save never ran (s22-clock starts before
chronyd and stops after it) and the RTC restarted from zero after a forced
power-off, so boots came up a day and a half behind. The save now waits for
chronyd; a last known time (saved with the offset and at shutdown) is the
floor at boot; and before chrony syncs, the cellular network's time (NITZ,
`qmicli --nas-get-network-time`, a local query with no transmission) sets
the clock and the offset.

## D016 The owner's account (2026-10-04)

`micro` (uid 1000), in `wheel` (sudo with its password), `audio`, `video`,
`input`, `network`; zsh; home on the SD card at /home/micro, so it survives
reflashing userdata. ssh with the same key as root. The password is set on
the phone with `passwd`, never in the build. root stays key-only over ssh.

## D017 Held packages: rebuilt from Artix in stage 30, checked with held-status (2026-10-04)

D013's three packages (polkit, pcsclite, btrfs-progs) are rebuilt from
Artix's PKGBUILDs in stage 30 (`ARTIX_BUILD` in config.sh: name and Artix
commit, in build order), with aarch64 added to `arch=()`; build-only
dependencies are installed `--asdeps` and removed after the builds. Upstream
PGP checks are skipped because the PKGBUILD, and with it every source
checksum, is pinned by the Artix commit. pcscd and polkitd no longer link
libsystemd.

`tools/held-status.sh` compares every IgnorePkg package on the phone with
ALARM's and Artix's current versions: `behind` (ALARM is newer; pacman keeps
skipping it), `rebuild` (and Artix has caught up: add or bump it in
`ARTIX_BUILD`), `artix-newer` (ALARM does not ship it, Artix moved on). Run
it before a `pacman -Syu` session; on 2026-10-04 it flagged pam 1.7.3 and
util-linux 2.42.4, which joined `ARTIX_BUILD` at the commits that moved them
to Artix's [system]. Rebuilt packages reach the phone with `pacman -U`.

## D018 Audio stack: PipeWire, with only WirePlumber rebuilt (2026-10-05)

PipeWire + WirePlumber + pipewire-pulse, run as OpenRC **user** services
(Artix's `pipewire-openrc`, `wireplumber-openrc`, `pipewire-pulse-openrc`,
in `/etc/user/init.d`, started at login by `pam_openrc`). Chosen over
bluez-alsa because the GUI will need it anyway; its ~50 MB is no concern.

- ALARM's pipewire packages declare systemd but only two plugins of
  libpipewire link libsystemd (`libspa-journal`, `libspa-v4l2`), neither
  loaded here. They are installed with `EXTRA_ASSUME_INSTALLED` (which now
  includes the soname form `libsystemd.so=0-64`) and held in IgnorePkg.
- WirePlumber is Artix's build (elogind instead of logind), without docs or
  GObject introspection (`packaging/artix-patches/wireplumber.sh`): both need
  doxygen, and nothing here uses the bindings.
- `libpulse` (PulseAudio clients) really links libsystemd; it came in with
  pipewire-pulse and is held. Rebuilt from Artix since 2026-10-07 (D032).
- `avahi-openrc` is installed because pipewire-pulse-openrc depends on it;
  the avahi daemon is not enabled. avahi (pulled in by libpulse) declares
  systemd-libs and is held in IgnorePkg too.
- Tested: Bluetooth A2DP (SBC) to a speaker. It stutters while WiFi
  transfers (shared 2.4 GHz radio); see the coexistence item in TODO.

## D019 WiFi and Bluetooth share one radio: three settings (2026-10-05)

The WCN3610's WiFi and Bluetooth run in the same WCNSS core on one 2.4 GHz
radio. Measured on the phone (ping to the router, LAN download):

| setting | before | after |
|---|---|---|
| BTC execution mode (kernel, wcn36xx) | 2, PTA only: A2DP stutters under WiFi traffic | **0, smart** (prima's default): A2DP smooth under a sustained download |
| WiFi power save | on: ping ~0.8 s | **off** (udev rule): ~70-80 ms with BT on, ~5-12 ms once the scan below is gentle |
| BlueZ background LE scan for trusted devices | kernel default, 30 ms every 60 ms: BT holds the radio about half the time | **11.25 ms every 1.28 s** (`ScanIntervalAutoConnect=2048`, `ScanWindowAutoConnect=18`): ping 5-12 ms with BT on, LAN download 0.39 -> 1.27 MB/s |

- Discovery (pairing) keeps BlueZ's fast default scan; only the background
  reconnect scan is gentle, so a trusted LE device takes up to about a
  second to reconnect.
- Relaying Bluetooth events to the firmware, as one might expect from
  Android, is unnecessary: prima's own WDA layer drops them ("TODO"); the
  firmware knows Bluetooth's state itself.
- Power save off costs battery while WiFi is associated; to be measured.
- Still open: about 10 Mbit/s against a 20-39 Mbit/s link rate; TX
  aggregation never starts (`agg_status`).

## D020 Reaching lk2nd's fastboot from Linux: a PMIC spare byte (2026-10-05)

lk2nd's fastboot used to need Volume Down within about a second of the stock
"unlocked" warning. `reboot bootloader` cannot help: the stock bootloader runs
first, reads the reboot reason from PON `SOFT_RB_SPARE` bits 7:2, scrubs it and
enters its own fastboot. IMEM's restart-reason word does not survive our reset
either (PSCI SYSTEM_RESET power-cycles the SoC; tested: the magic came back as
SRAM noise).

So the kernel's `qcom-pon` gets a one-shot switch, `lk2nd_fastboot` in sysfs,
that writes a magic byte (0x6c) to PON `DVDD_RB_SPARE` (0x88D). It is a PMIC
register, so it survives the reset, and neither stock bootloader code path reads
or clears it (it read 0x00 on a normal boot). lk2nd checks it next to the reboot
reason, clears it and stays in fastboot. The kernel also clears a leftover magic
at probe, so the switch never outlives one boot even with an lk2nd that does not
know it. `reboot-lk2nd` sets the switch and reboots normally (OpenRC's reboot
cannot pass a reboot argument, so a reboot-mode string would not work).

Kernel 8a08db982 (BENCkernel-CAT 7.1.3-9); lk2nd f7baf03.

## D021 WiFi power save follows the lid; lid state re-applied after resume (2026-10-05)

Measured with an inline USB meter (screen on, battery full): power save on
saves about 17 mA (275 -> 258 mA total), but with it on the firmware holds
packets (~0.8 s pings, D019). So s22-lidd (0.2) gained hooks, executables in
/etc/s22-lidd.d run with `open` or `closed`, and `10-wifi-powersave` turns power
save off with the lid open and on with it closed, unless a Bluetooth audio sink
is connected. After a suspend the display driver powers the panel up whatever
the lid says, so s22-lidd (0.3) re-applies the lid state after every resume,
detected as a jump between CLOCK_BOOTTIME and CLOCK_MONOTONIC (or on SIGUSR1).

## D022 tqftpserv serves the modem's files, as stock's tftp_server (2026-10-05)

Stock runs `tftp_server`, which serves the `/vendor/rfs/msm/mpss` tree to the
modem over QRTR. We did not, and the modem reported its operating mode as
"shutting-down" at every boot. Running linux-msm's `tqftpserv` showed what the modem asks for: at
boot the modem writes `/readwrite/server_check.txt` and `mcfg.tmp` and reads
its MCFG config indexes `modem_pr/mcfg/configs/mcfg_{hw,sw}/mbn_*.dig` from
`/readonly/firmware/image/`. With tqftpserv running and the `modem_pr` tree
(from the modem partition) installed next to the modem firmware, the reads
succeed. MCFG is also where carrier configs (VoLTE) live. Correction: serving
TFTP does not change the reported mode; the modem stays "shutting-down" until
something sets it online (on stock, the RIL at boot; here `qmicli
--dms-set-operating-mode=online`). tqftpserv is a local package, started before rmtfs (which boots
the modem); `modem_pr` is installed with the other firmware and, like it,
never published. It did not make the modem power-collapse (no SIM): to be
re-checked with a SIM.

## D023 Outer display: s22-outerd status screen (2026-10-05)

The 128x128 outer panel (fb1, panel-mipi-dbi, on/off GPIO backlight) shows a
status screen while the lid is closed: lit for a few seconds when the lid
closes, when the side, volume or power key is pressed, and when a notification
arrives; dark otherwise (backlight off, panel blanked), since the backlight is
the cost. It shows a 12-hour clock with AM/PM and a blinking colon (or
24-hour), the date with the year, WiFi bars, a Bluetooth mark while a device is
connected, battery with a charging colour, and the newest notification as a
one-line ticker. Settings for a future settings app are a plain key=value file
re-read on change (`/var/lib/s22-outer/settings`: clock, blink, timeout); the
wallpaper is a binary PPM at `/var/lib/s22-outer/wallpaper.ppm`, any size,
scaled to cover. Notifications are files in `/run/s22-notify` (sticky, world
writable), posted with `s22-notify`. Font: the Linux kernel's 8x16 console font
(GPL-2.0). s22-outer still brings the panel up at boot; s22-outerd takes it
from there. Inputs are read, not grabbed, so s22-t9d and elogind are unaffected.

## D024 Notifications as a system; one owner for the lid-closed keys (2026-10-05)

Notifications are files in `/run/s22-notify` with `source=` and `text=` lines,
posted with `s22-notify -s SOURCE TEXT` by any user. Consumers watch the
directory independently: `s22-notifyd` vibrates with a per-source pattern
(`/var/lib/s22-notify/vibrate`: on,off,... ms, `none`, `strength=`; re-read on
change, bursts within 1 s merged), `s22-outerd` shows them. A future GUI or
settings app reads and writes the same files.

With the lid closed only the side key and the volume keys are reachable (power
is the End key, on the keypad). Every program can read those keys, so one owns
them, s22-outerd: side short = wake / next notification (wrapping, "2/3"
badge), side long (600 ms, fires while held) = dismiss the one shown; volume
short = media volume, long = next/previous track, also with the lid open. Media
actions go through a hook (`/etc/s22-outerd.d/media`), so a music player
can take the volume keys over without touching the daemon; the default hook
uses the logged-in user's PipeWire (wpctl) and MPRIS (playerctl) and prints the
new volume, which s22-outerd shows as a bar for 2 s. The WiFi signal comes
from nl80211 (`iw`), since /proc/net/wireless needs wireless extensions; the
WiFi icon is arcs, and signal bars are kept for the cellular signal.

## D025 Text messages: s22-smsd speaks QMI WMS itself, into the notification system (2026-10-05)

ModemManager was the first choice (built without systemd from Artix's
PKGBUILD), but it refuses this modem: "Failed to find a net port in the QMI
modem". Its QMI modems need a network port, and the bam-dmux netdevs only
appear once the modem raises its SMSM power-collapse bit, which this
firmware never does (online or not). The AT port (wwan0at0) has no SMS
commands, and qmicli's WMS support stops at routes. So ModemManager is not
built or installed; it can come back if bam-dmux ever appears.

`s22-smsd` (Python, libqmi through GObject introspection, about 38 MB
resident) opens QRTR node 0 itself. At start it puts the modem online
(DMS; `-n` leaves it alone), routes every MT message class to the modem's NV
storage with store-and-notify (so the modem acks to the network) and turns on
new-message indications. Each stored MT message, from an indication or from a
sweep at start and every 5 minutes (NV and SIM storage), is read raw,
decoded (3GPP SMS-DELIVER: GSM 7-bit with the extension table, UCS-2,
concatenated parts kept in `/var/lib/s22-sms/parts` until complete or an hour
old; MMS notifications become a "not supported yet" line), appended to
`/var/lib/s22-sms/inbox` (tab-separated time, sender, text; group wheel), and
posted as an `sms` notification (vibration pattern, outer display); only then
is it deleted from the modem, so its storage never fills.

It never opens a WDS client, so it never starts mobile data. The SIM's plan
has 2 GB of data; the modem's own autoconnect setting is checked once the SIM
is in. Tested without a SIM by writing PDUs into NV storage (raw write, tag
set to MT not read): single, UCS-2 and out-of-order two-part messages.

## D026 The modem's IMS asks the AP for its PDN: s22-imsd answers (2026-10-06)

The modem never power collapsed (RPM master stats: 0 MPSS shutdowns, ever)
and stayed RRC connected, transmitting. Its own debug messages, read over
DIAG from Linux, said why: the modem-side IMS stack's data connection
manager asks the AP to bring up the IMS PDN ("QMI send failed !! IMS PDN
cannot be brought up on AP"), and retries forever when nobody answers.
Android's imsdatadaemon hosts that QMI service (770, "IMS data service").

`s22-imsd` hosts service 770 on QRTR. The modem connects at once and sends
three setup requests (answered with plain success) and a PDP Activate
request naming the APN, IP family and WDS profile ("ims", IPv6, profile 2
with a T-Mobile SIM). s22-imsd does netmgrd's DPM port and raw-IP data
format on bam-dmux, starts that WDS profile, and returns the address in
the PDP Activate indication (layouts as in libqmi's IMSDCM definitions).
Result: the transmitter idles and the modem power-collapses about every
0.6 s, asleep ~98% of the time.

Only the IMS PDN is brought up, never internet data; the user approved it
as a standing exception to asking before cellular traffic, since every
phone keeps this PDN up. VoLTE registration itself is not complete yet
(IMSA: not registered); the network here has no CS fallback for voice.

The pinned package list now derives every package built here (LOCAL_PKGS
and ARTIX_BUILD) instead of a second hand-kept list, which had drifted:
s22-smsd was never pinned on the phone. `tools/sync-ignorepkg.sh` writes
the same line into a running phone's pacman.conf.

## D027 Battery log on the SD card, power-off at 5% (2026-10-06)

The PM8916 gauge here runs in voltage mode: it reports voltage and a
capacity estimated from it, but no current, so drain can only be measured
over time. `s22-batteryd` appends a CSV line every 5 minutes to
`/home/s22-battery/battery-YYYY-MM-DD.csv` on the SD card (time, uptime,
charger, status, capacity, voltage, lid, suspends, the modem's power
collapses and time asleep from the RPM master stats, and the RPM's vlow and
vmin counts), which makes overnight drain and sleep behaviour readable
afterwards without a USB connection.

It also powers the phone off cleanly when the battery runs out: while
discharging, at or below 5% or within 50 mV of the battery's minimum design
voltage (the percentage is only an estimate), confirmed over three checks a
minute apart so a dip under load does not count. It posts a notification
from source `battery`, waits 30 s (plugging in cancels), syncs
and runs `poweroff`.

It fires once: a marker file (`/var/lib/s22-battery/low-poweroff`) keeps it
disarmed after the next boot until the battery has been charged back to 20%,
so charging from empty can never become a power-off loop (it also never
acts while the charger is connected).

Capacity smoothing (s22-battery 0.3, 2026-10-06): the voltage-mode gauge's
capacity jumped with load, 55% to 85% on plugging in (the charge current
times the battery resistance lifts the voltage; the mainline driver counts
that as charge) and in 6-8% steps while discharging. Like downstream's
VM-BMS driver (GPL; its reported SoC is rate-limited and blends toward the
new value while charging), s22-batteryd keeps its own capacity: only
falling while discharging (at most 1%/min), only rising while charging (at
most 0.5%/min, 100% when the charger reports full), a fresh hardware OCV
reading as the stronger target, kept across reboots. It is published in
/run/s22-battery/capacity (s22-outerd shows it, the 5% check uses it) and
logged as est_capacity next to the driver's. Downstream's other advantages
remain to do: ADC calibration against the PMIC's 0.625/1.25 V references,
and true OCV readings, which the PMIC only takes while the system sleeps.

IMS registration (2026-10-06): the PDP Activate indication needs the
result field (0x02, zero when up). Without it the modem brought the PDN up
through us, never attempted registration, and released the PDN after
about 15 minutes, in a 20-minute cycle that kept the modem busy most of
the time (asleep 47% of the night). With it IMS registers within seconds
(IMSA: registered over LTE; voice, SMS and UT full service) and the modem
is asleep ~98% of the time. Field meanings as documented by 81voltd (GPL,
Richard Acayan), which also gives the 0x21 response (0x10 = 0xfa). The
AP-side modem proxy and the link-local call (-P/-Q, experimental) turned
out not to be needed for registration.

## D028 Voice calls without a GUI: s22-calld (2026-10-06)

With IMS registered (D026), calls are VoLTE. Call control is the modem's
QMI Voice service (dial, answer, end, all-call-status indications). Call
audio never passes through Linux: the modem's voice session runs on the
ADSP, started by opening the hostless 8 kHz VoiceMMode1 PCM (q6voice-dai,
added to the sound card as on the Nokia 2780) and routed with the q6routing
voice mixers to Primary MI2S (earpiece) and from Tertiary MI2S (the keypad
microphone, AMIC1).

`s22-calld` (Python, libqmi) does it without a GUI, the way a flip phone
does: an incoming call rings (repeating vibration) and posts a `call`
notification; opening the lid or the Call key (which s22-t9d turns into
Enter) answers; closing the lid or the End key (the power key, whose
short press logind ignores) ends or rejects. While a call is up the
routing is set and the voice PCM is held open (arecord/aplay); both are
undone when the last call ends. The voice PCM is hostless: reads and
writes fail with EINVAL, so arecord/aplay exit at once and close it (no
audio on the first test call); `s22-pcmhold` (C, alsa-lib) opens both
directions, starts them and only holds them. `s22-call dial NUMBER | answer | hangup |
status` talks to the daemon over /run/s22-call.sock (root and wheel); the
daemon never dials by itself. -L turns the lid actions off.

## D029 The GUI: s22-wm, s22ui and small apps (2026-10-06)

A phone-sized GUI written here rather than a desktop's pieces: one screen
of 480x640, a keypad and no pointer want little of a desktop compositor and
toolkit, and both would cost memory the phone does not have.

- **s22-wm** (C, wlroots 0.20, cut down from tinywl): every window
  fullscreen, the newest on top. End and Home go home (the launcher), Recents
  goes to the previous window. The lid and an idle timeout switch the display
  off; the key that wakes it is not passed on. A control socket
  ($XDG_RUNTIME_DIR/s22-wm.sock: home, raise APP_ID, wake, list) lets apps
  and daemons raise windows. Pixman renderer on the DPU (card0); the outer
  panel stays s22-outerd's.
- **s22ui** (C, static library): wl_shm buffers, xdg-shell, keys as evdev
  codes, FreeType text (DejaVu), and the pieces every app shares (header with
  clock and battery, key hints bar, scrolling list, settings file).
- **Keypad:** s22-t9d keeps typing multi-tap text for programs that only
  know a keyboard (foot), and passes keys unchanged to our own apps
  (app_id "s22-..."), which do their own text entry. s22-wm tells it which
  through /run/s22-t9/gui on every focus change.
- **Settings:** one file, /var/lib/s22/settings (key=value, group wheel,
  setgid directory), read by whoever needs a key: s22-wm the screen timeout,
  s22-outerd the clock and its timeout, the s22-settings service the
  hardware state at boot. No settings daemon and no setuid helper: the
  Settings app writes brightness (udev rule, group video) and rfkill (group
  rfkill) itself.
- **Session:** seatd (group video) and s22-wm as the owner (service
  s22-gui) on tty1; the getty under it comes back if the GUI stops.

Apps are listed by .app files (/usr/share/s22/apps, ~/.local/share/s22/apps).
Roadmap: docs/GUI.md.

## D030 XO is voted through the RPM; idle hardware lets go of it (2026-10-06)

The RPM may shut XO down and minimise VDD (vlow/vmin) whenever every
master sleeps. Linux used to take XO from a fixed board clock, so it never
voted XO in the RPM sleep set: with the RPM-notifying cluster idle state
the RPM stopped XO under running peripherals (outer display SPI timeouts,
a wedged DPU), and that state had to stay off. BENCkernel-CAT 7.1.3-27
instead feeds gcc and the other real XO users from the RPM's XO clock,
which votes in both sets while enabled, and keeps the cluster state.

That is only worth it if idle hardware drops its vote. Three changes do
that: the A53 PLL stays on the board clock (an RPM clock there deadlocked
boot: the SMD edge maps APCS through syscon, which takes the PLL), the
digital codec's AHB stand-in goes back to the board clock (msm8917 has no
such GCC clock; the ADSP votes for LPASS), and the ChipIdea USB glue
runtime-suspends with no cable and gates its clocks (the system clock
runs from GPLL0, which held XO). With the lid closed and the cable out
nothing in Linux holds XO: in 10 minutes the RPM reached VDD minimisation
8137 times, 198 s in all, and five lid cycles left the outer display
clean. USB keeps XO on while plugged in, so sleep is measured unplugged.

## D031 Fewer idle wakeups: sensors on demand, lazy RCU, no PSI (2026-10-06)

With deep sleep working (D030), each VDD-minimisation stretch lasted only
about 24 ms: the CPUs woke some 14 times a second. A trace (lid open, USB
in) found, besides timers that only follow from the rest:

- s22-sensord streamed all four sensors nonstop (18 reports/s, each waking
  the ADSP and the CPUs) though nothing read them. Sensors now run on
  demand: a client of `/run/s22-sensord.sock` (root:wheel) holds a sensor
  at a rate, the highest wanted rate is used, and a sensor stops when its
  last holder lets go or disconnects. `s22-sensor` holds sensors from a
  shell and prints readings. SMGR stops a report on a delete that repeats
  the report's fields (the bare delete is refused, error 17).
- RCU's grace-period thread woke ~38 times/s: lazy RCU (callbacks offloaded
  on all CPUs, lazy ones batched), kernel 7.1.3-28.
- PSI averaged pressure every 2 s in each of ~33 OpenRC service cgroups
  (~7/s); nothing on the phone reads it: `psi=0`.
- vmstat every second and proactive compaction: `vm.stat_interval = 10`,
  `vm.compaction_proactiveness = 0`.
- The router advertises IPv6 every 3 s and dhcpcd ran its whole hook script
  (about ten processes) for each: an enter-hook skips ROUTERADVERT runs.
  dhcpcd still configures the address; IPv6 DNS from advertisements no
  longer reaches resolv.conf, DHCPv4 DNS does.

WiFi power save stays as D021 has it (on with the lid closed).

## D032 Video and 3D: Venus, the GPU's speed bin, libpulse from Artix (2026-10-07)

- **Venus** is enabled (BENCkernel-CAT 7.1.3-30) with the phone's own
  `venus.mbn` and the `venus_mem` carve-out: H.264, HEVC and VP8 decoding,
  H.264 encoding, through V4L2 (ffmpeg's and mpv's `v4l2m2m`). It probing
  also lets GCC's sync_state finish.
- **GPU** (Adreno 308, Mesa's freedreno "FD306A": OpenGL ES 3.0, GL 3.1).
  This QM215 is fused as speed bin 3, 400 MHz at most; msm8917.dtsi offers
  up to 598 MHz, which locked the GPU up under load. The board DT keeps
  270 and 400 MHz only: 19.2 MHz goes too, as devfreq parks the idle GPU
  there and the first heavy frame after idle then outlasts hangcheck.
- **libpulse** links libsystemd in ALARM, so mpv and ffmpeg (PulseAudio
  clients) could not start. It is built from Artix's PKGBUILD, client side
  only (`packaging/artix-patches/pulseaudio.sh`: `-D daemon=false`, no X11,
  meson's auto features detected rather than all required as arch-meson
  would), pinned in `ARTIX_BUILD`.
- **mpv** plays through s22-wm, which now offers `wp_viewporter`, with
  `--hwdec=v4l2m2m-copy --vo=wlshm`: 360p plays smoothly, 720p drops most
  frames in the software scaling and copy (s22-wm renders with pixman; a
  dmabuf path would remove the copy). ffmpeg 9's v4l2m2m wrapper aborts at
  end of stream; mpv with `--end` short of it is fine. mpv needs hidapi,
  whose systemd-libs dependency is only libudev here (installed with that
  dependency assumed), and pipewire-jack rather than jack2 (whose jackd
  links libsystemd). Not in the image yet.

## D033 The outer display's SPI pins are shared with the ADSP (2026-10-08)

Since sensors went on demand (D031), the first outer-display enable after
boot could wedge its SPI controller (BLSP2 QUP2) until it was bound again.
TLMM showed why: GPIO 22 and 23, the bus's chip select and clock and also
this QUP's I2C pins, read back as pulled-down GPIO outputs while Linux's
pinctrl still listed `blsp_spi6`. Nothing on the Linux side writes that;
the ADSP's sensor framework does, apparently parking what it takes for
its own I2C bus whenever its sensors go idle (with them streaming, as
before D031, it never did). Neither the registry we serve (no group holds
22 and 23) nor anything we may change controls it: it comes from the
ADSP's signed firmware or configuration.

So Linux takes the pins back before every use: BENCkernel-CAT 7.1.3-32's
spi-qup selects its sleep pin state on runtime suspend and its default
state on resume, and a transfer that still times out resets the
controller, reapplies the pins and retries once. Eighteen hours with lid
cycles, no timeout. s22-sensord's boot sensor pulse (a stopgap from before
the cause was known) is off by default (0.4).

## D034 The GPU composites; one status bar for every window; s22-celld (2026-10-08)

- **s22-wm renders with GLES** (freedreno) by default since s22-gui 0.22,
  pixman being the fallback (`S22_WM_RENDERER`). With the GPU capped at its
  fused 400 MHz (D032) it is stable, and it is what lets mpv show 720p
  video smoothly (GPU scaling, `/etc/mpv/mpv.conf`); s22-wm also gained
  idle-inhibit, since a GPU-drawn client stops while the display is off.
- **The status bar is the system's, not each app's:** s22-statusbar draws
  it (WiFi, cellular, clock, battery) as a layer surface that reserves its
  26 px, and s22-wm gives windows the rest, so foot and any other program
  sit below it. An s22ui app draws only its title bar, centred.
- **s22-celld** follows the modem's registration and signal through QMI NAS
  indications and writes `/run/s22-cell/state` for both status bars (the
  outer display shows cellular bars next to the WiFi arcs). After a network
  drop the modem stayed camped on another operator's cell in emergency-only
  service for 20 minutes: short of full service for 5 minutes, s22-celld
  cycles the radio (at most every 15 minutes; an automatic attach, agreed
  with the user), and when service comes back it has s22-imsd offer the
  IMS data service again (SIGHUP), which the modem's IMS otherwise never
  asks for.

## D035 Mobile data: WiFi first, holds for carrier-only jobs (2026-10-08)

The data plan is small, so cellular must never carry ordinary traffic
while WiFi works, yet MMS reaches only the carrier's network and has to
work with WiFi up. s22-celld owns the internet PDN (WDS profile 1) on
bam-dmux channel 1, beside s22-imsd's IMS on channel 0. The modem opens a
channel, and the kernel makes its wwan interface, only once the channel's
DPM port is open (DATA5_CNTL is channel 0, DATA6_CNTL channel 1). The
network is IPv6-only: an IPv4 call gets a 464XLAT placeholder address
(192.0.0.2) that needs a CLAT on the phone, so it is dropped and IPv4-only
names reach the phone as DNS64 addresses. First test (2026-10-08): a hold
on wwan1 with WiFi up, 4/4 IPv6 pings to google.com at ~75 ms, WiFi still
the only default route.

- **Internet mode** only with `mobile_data=on` (Settings, default off),
  full service, and no WiFi default route for 10 s. Its default routes have
  metric 4000, behind WiFi's, and its DNS goes through openresolv. Once
  WiFi has a default route for 10 s the link goes back down (or to holds).
- **Holds:** a program connects to `/run/s22-cell/ctl` (root:wheel 0660)
  and sends `hold NAME`; the link comes up without default routes in the
  main table and the answer names the interface. Only sockets bound to it
  get out, through a policy rule (`oif` → table 1001). Closing the
  connection releases the hold. MMS and the Settings LTE ping use this.
- Router advertisements and autoconf are off on the interface and dhcpcd
  ignores `wwan*`, so nothing else can add a route through it.

## D036 VPNs: Tailscale always on, Mullvad as a switch (2026-10-08)

The user wants both: Tailscale for their tailnet, always on, and Mullvad
(a standalone account) for privacy when they choose.

- **Tailscale** is ALARM's package with our own OpenRC service
  (`tailscaled`). s22-vpnd logs it in with `--accept-dns=false` when it
  needs a login and shows the URL in Settings → Network & internet → VPN,
  so the tailnet's DNS never replaces the system's (MagicDNS names are
  not resolved for now).
- **Mullvad** is plain WireGuard, no Mullvad app: `wg-quick` with a config
  from Mullvad's WireGuard config generator at
  `/etc/wireguard/mullvad.conf`, up while the `mullvad` setting is on.
  Its DNS goes through openresolv while it is up.
- **Routing composes by rule priority:** Tailscale's rules (5210–5270)
  come before wg-quick's, so the tailnet (100.64.0.0/10) stays on
  Tailscale, Tailscale's own WireGuard traffic and the LAN stay outside
  Mullvad, and everything else goes through Mullvad. s22-celld's hold rule
  (1001) comes before both, so a held cellular link stays cellular.
- **Logins happen on the phone** (Settings → VPN, through s22-vpnd's
  control socket, root:wheel): Tailscale's login link shows as a QR code
  to scan with another phone, or an auth key can be typed; for Mullvad
  the account number is typed, and s22-vpnd makes a WireGuard key,
  registers it as one of the account's devices (Mullvad's accounts API),
  picks a relay in the chosen country (their public relay list) and
  writes the config. When the account already has its maximum of devices,
  Settings lists them and removes the one picked (after a second press)
  to make room. Logging out removes this phone's device. The account
  number and key stay on the phone (`/var/lib/s22-vpn`, 0600).
- Undecided: Mullvad over cellular. The carrier's network is IPv6-only
  (D035); Mullvad's relays have IPv6 addresses too, so it can work there
  once the endpoint follows the network in use.

## D037 Bluetooth through s22-btd; one T9 indicator, in the status bar (2026-10-08)

- **s22-btd** follows BlueZ over D-Bus and writes `/run/s22-bt/state`
  (powered, discovering, connected, pairing codes) and `/run/s22-bt/devices`
  (paired devices, and named ones the current scan found) for Settings and
  both status bars. Settings sends it commands (scan, pair, connect,
  disconnect, forget) through `/run/s22-bt/ctl` (root:wheel). Its pairing
  agent (DisplayYesNo) only answers pairings started from Settings: a code
  to compare is shown in Settings and waits for the user, an old device's
  PIN is 0000, and anything else is refused, since the phone never asks
  to be discoverable. A paired device is trusted, so its services connect.
- **Bluetooth icons:** grey when Bluetooth is on, blue while a device is
  connected, on the outer screen and after the cellular bars in the GUI.
- **The keypad indicator is the status bar's** for every window: s22-t9d's
  for windows that take typing as text (foot), and for our own apps, the
  one in front writes its entry's state (`$XDG_RUNTIME_DIR/s22-entry`)
  instead of drawing it in the window.
- **Home closes the apps** (not the Shell): each starts on its first page
  next time, and nothing piles up in memory.

## D038 Phone, Contacts and Messages share one address book (2026-10-08)

- **Contacts are one vCard file**, `/var/lib/s22/contacts.vcf` (group
  wheel, 0660), so they move to and from other phones and computers as
  they are. s22ui reads it (again whenever it changes) for every app:
  lookups by number compare the last ten digits, so +1 and national
  forms match, and smart dial matches names by the letters on the keys.
  The Contacts app edits it and imports `.vcf` files from the home folder
  or Downloads (numbers already there are skipped). s22-calld reads it for
  the names in call notifications.
- **The Phone app is a client of s22-calld**, which still owns calls,
  audio and the ring: it adds a state file (`/run/s22-call/state`), a call
  log (`/var/lib/s22-call/log`, group wheel), DTMF tones, mute (the voice
  capture mixer), and reject. A ringing call's notification (source
  `call`) is opened at once by s22-shell as the call screen, instead of a
  banner; a missed call leaves a `missed-call` notification that opens
  Recents. Nothing dials except on a key press in Phone (or `s22-call`).
- **Links tie them together:** `tel:` (Phone calls), `sms:` (Messages),
  `contact:new:NUMBER` and `contact:list` (Contacts), `call:incoming` and
  `call:recents` (Phone). Messages shows names, and its Call key calls.

## D039 Call keys are for calls; the call microphone is tuned in Settings (2026-10-08)

- **Call and End keys belong to calls.** End (the power key) no longer goes
  home: s22-calld hangs up or declines with it, s22-wm only lets it wake
  the display, and a long press is kept for a power menu. Only Phone's
  keypad and call screens use the Call key; Home goes home. After a call
  placed from Phone the keypad comes back empty; a call that rang while
  another app was in front closes Phone when it ends, back to that app.
- **The call microphone's gain is a setting** (`call_mic_adc`, 6 dB steps
  of analog gain; `call_mic_digital`, the decimator, 84 = 0 dB) that
  s22-calld applies at each call. Developer options has both and a test,
  s22-mictest: it records the keypad microphone through the call's path
  and gains, says how loud the loudest moment was (dBFS), and plays it on
  the earpiece at the call volume, so the gain can be set by ear and by
  number without making a call.
  Tried by ear on calls: +24 dB analog and +4 dB digital are the defaults
  (the first +6 dB was too quiet for the other side).

## D040 Sound through PipeWire; the amplifier stays at stock (2026-10-08)

Notification and ring sounds must mix with whatever else plays, and need
a volume apart from media. Tried and found on this card:

- **dmix does not work:** through it the card's stream never moves
  (hw_ptr stays 0) with dmix's own buffer choice, and hangs with others
  too, while direct playback (also mmap) runs.
- **PipeWire works, in 16-bit:** it picks S24_LE, which the audio DSP plays
  about 48 dB too quiet (as if the 24 bits sat at the top of the 32);
  WirePlumber forces S16LE at 48 kHz on the card's output.

So the sound server is PipeWire, WirePlumber and pipewire-pulse, run as the
GUI's user in the GUI's runtime directory by its own service (`s22-audio`,
with a private D-Bus session), not by s22-gui, so restarting the GUI keeps
sound. The card's output is named `s22.speaker` and suspends after 5 s
idle so the audio DSP sleeps. The default output is "Media", a loopback
into it whose volume is the media volume; `s22-play` (notifications,
ringtones, previews; root's daemons connect to the GUI user's PipeWire)
plays into `s22.speaker` directly at its own volume, the ring and
notification volume. The amplifier ("Speaker Volume") stays at stock's
-4 dB, except for the call volume on speakerphone. Bluetooth audio can
come through the same PipeWire later.

## D041 Sounds: AOSP's, picked per app and per contact (2026-10-08)

- **The sounds are AOSP's** (frameworks/base/data/sounds, Apache-2.0 per
  its Android.bp), packaged as `aosp-sounds` from AOSP's own source at a
  release tag, never from the phone's stock image. Gitiles makes its
  tarballs on the fly, so each file is checked against a manifest in the
  repository. One directory a kind (ringtones, notifications, alarms):
  the classic top-level sounds first, later ones whose names are free
  (case aside), no `_48k` resamples, and none of the same audio under
  another name: AOSP ships many twice (ANDROMEDA and Andromeda, Pyxis,
  Sedna and Ring_Digital_02, ...), found by decoding every sound and
  comparing waveforms, listed in `packaging/aosp-sounds/duplicates`. 179
  sounds.
- **Choosing:** Settings > Sounds has sounds on or vibrate only, the ring
  and notification volume (s22-play's) and the media volume, the ringtone,
  the default notification sound and one for each app that notifies
  (`sound_SOURCE`: a file, "silent" or "default"). A contact may have its
  own ringtone and text tone (`X-S22-RINGTONE`, `X-S22-TEXTTONE` in its
  vCard), set in Contacts. The pickers play each sound as it is chosen.
- **Playing:** s22-notifyd plays a notification's sound (the sender's text
  tone, else the app's, else the default) with its vibration, never during
  a call; s22-calld loops the caller's ringtone (else the phone's) while a
  call rings, and a volume key silences it.

## D042 Vibration patterns, chosen like sounds (2026-10-08)

A pattern is "on,off,on,..." in milliseconds (16 steps, 5 s each at
most), "none", or "default" (the next setting decides), stored like the
sounds: `vibrate` (on/off), `vib_strength` (1-15), `vib_ring` (repeated
while a call rings), `vib_notify`, `vib_SOURCE` per app, and per contact
`X-S22-CALLVIB` and `X-S22-TEXTVIB`. Settings and Contacts pick from
presets (notifications: Default, Short, Long, Double, Triple, Heartbeat,
Rapid, Morse V; ringing: Standard, Pulses, Heartbeat, Rapid, Long) and up
to four recorded ones (`vib_custom_N`): the recorder vibrates while OK is
held and keeps the on and off times as they were. Every choice is felt as
it is picked (`s22-vibrate`, group input). s22-notifyd takes the sender's,
the app's, then the default (its old per-source file last); s22-calld
plays the caller's, else vib_ring, step by step while it rings.

## D043 The power menu: End held, through a single sudo rule (2026-10-08)

End held a second (lid open) asks s22-shell for the power menu: power
off, reboot, and reboot to lk2nd's fastboot (`reboot-lk2nd`, D020). The
GUI runs as its user, so the menu runs `sudo -n /usr/local/bin/s22-power
off|reboot|lk2nd`, and `/etc/sudoers.d/20-s22-power` lets group wheel run
that script, and nothing else, without a password (wheel's own rule still
asks for one). A short End press still only hangs up (s22-calld) and
wakes the display.

## D044 A PIN lock screen, through the session-lock protocol (2026-10-08)

- **s22-wm is the lock's keeper** (ext-session-lock-v1, wlroots'
  `wlr_session_lock_v1`): while locked, only the lock surface shows and
  takes keys and touch; a lock client that dies leaves the session locked
  (blank) and s22-wm starts `s22-lock` again. With a PIN lock set it locks
  `lock_after` seconds after the display goes dark (lid or timeout; never
  during a call), and at its start. A call's Phone window shows over the
  lock, to answer and talk; the lock is back when the call ends. Home,
  Recents and the side key's press do nothing while locked; the flashlight
  hold and the volume keys still work.
- **s22-lock** shows the clock and the PIN as dots (keypad digits, raw);
  after 5 wrong PINs it waits 30 s, doubling each time after. End held
  reaches it as KEY_POWER: power off or reboot, never lk2nd's fastboot.
- **The PIN** is kept only as a salted hash (`lock_pin`, crypt(3),
  libxcrypt's default method) in the settings, which group wheel reads;
  Settings > Security sets it (twice), changes or removes it only with the
  current one, sets lock_after, and locks now.
- Not a strong boundary: root over USB, a reflash, or reading the hash and
  trying all 4-digit PINs offline get past it; it keeps a found or taken
  phone's GUI closed.

## D045 Cellular on and off (2026-10-08)

`cellular=off` (Settings > Mobile network, a quick settings tile) puts the
modem's radio in QMI low-power mode, off the network: no calls, texts or
data, while WiFi and Bluetooth go on. s22-celld applies it (and again at
its start), the registration watchdog leaves the radio alone meanwhile,
and the status bar hides the cellular icon (`radio=off` in its state).
`cellular=on` puts the modem back online; service came back within a
second in the first test, and IMS through s22-imsd's usual re-offer.

## D046 pacman-sd and sd-run, as built (2026-10-08)

D006's plan, made, then reworked the same day so install scripts and hooks
run:

- **The transaction runs in a sandbox** (bubblewrap, as root): the card's
  layers (`/home/.pacman-sd/layers/{usr,etc,var}`) are writable overlays
  on the phone's /usr, /etc and /var, and pacman installs to `/` there.
  Install scripts and hooks run with the phone's shell and tools; what
  they write lands on the card. The card's database is its own
  (`/home/.pacman-sd/db`); the phone's repositories, keys and synced
  databases are shared, downloads kept on the card.
- **Nothing is duplicated, nothing of the phone's overwritten:** every
  phone package and all it provides is `--assume-installed`, and so is
  what the phone runs without on purpose (systemd, systemd-libs,
  dbus-broker, device-mapper, ... at the repositories' versions, with
  their provides), so packages asking for them never pull them onto the
  card; a target the phone has is refused; pacman refuses to overwrite a
  phone file it does not own (it caught systemd-libs over libelogind's
  files before that list existed). A hook's `ld.so.cache` on the card is
  dropped, so it can never hide the phone's libraries after an upgrade.
- **Running:** `sd-run` lays the card's layers *under* the phone's
  (read-only overlays), so the phone's files always win, whatever a script
  wrote on the card. pacman-sd writes a command per program in
  `/home/.pacman-sd/bin` (on PATH after the phone's) and a home screen tile
  per desktop application in `/home/.pacman-sd/apps` (the launcher reads
  it), both through sd-run.
- Hooks share the phone's sockets: a udev rule reload, say, reaches the
  phone's udevd. Harmless so far; worth knowing.
- Tried: tcsh (its install script added it to the card's /etc/shells, not
  the phone's), imv (no tile: it hides its entries), zathura (47 packages,
  every hook ran, a tile, its window opened).

## D047 A session bus for the GUI's user; a valid machine ID (2026-10-08)

GTK programs (from the SD card, D046) found no session bus: the image's
`/etc/machine-id` lacked its newline, which D-Bus requires (32 hex digits
and a newline), and there was no bus to find anyway. Now:

- the image writes the ID with its newline, and s22-settings adds a
  missing one at boot (keeping the ID), or makes one if there is none;
- `s22-bus` runs one session D-Bus for the GUI's user at
  `/run/s22-gui/bus`, as a systemd login would; s22-audio (PipeWire,
  WirePlumber, pipewire-pulse) and s22-gui both use it
  (`DBUS_SESSION_BUS_ADDRESS`), instead of PipeWire's private bus from
  `dbus-run-session`, so every program the GUI starts shares it.

Switching over on the phone left the old session (bus, PipeWire) running
without its service: stopping a `dbus-run-session` wrapper does not stop
what it started. The session script now is the supervised process and
takes its children with it when stopped.

## D048 Game Boy and NES: one libretro frontend, two apps (2026-10-08)

The keypad has enough buttons for the Game Boy (and Color) and the NES,
and no more, so those two. One small frontend, `s22-emu` (s22-gui),
loads an Arch package's libretro core with `dlopen`: gambatte for GB/GBC,
nestopia for the NES. These two are accurate, and fast enough here
(measured on the phone: about 1 ms and 7 ms per frame of the 16.7 ms there
is). It shows as two home screen apps, Game Boy (`s22-emu gb`) and NES
(`s22-emu nes`). Their app IDs start "s22-", so the keypad comes in raw.

- **Games:** the user's own, in `~/Games/gb` and `~/Games/nes` on the SD
  card. None are shipped.
- **Saves:** go to `~/Games/saves`.
  - The battery memory (and the clock) is written when it changes, every
    few seconds of play and at every pause.
  - One save state is kept from the menu.
  - A resume state is written when the app closes; Home closes apps.
    Opening the game again continues from it.
- **Picture:** the largest rectangle that fits the window, at the
  aspect ratio the core reports, pixel aspect included (NES 8:7). The
  Game Boy comes out at exactly 3x (480x432). For scales that are not
  whole numbers (NES, 1.875x by 1.64x), each screen pixel takes the game
  pixels it covers, by area. Pixels stay even and sharp, with one soft
  screen pixel at their edges. Plain nearest-neighbour would mix 1- and
  2-pixel columns, and bilinear blurs.
- **Wakelock:** idle-inhibit while a game runs, so the display stays on
  (s22ui gained `ui_inhibit_idle`). The game pauses into its menu, and
  lets go of the inhibit, when the window loses the keyboard or stops
  being shown. Not shown means the display is off, the lid is shut, the
  lock screen is up, or another window is on top. The compositor sends
  no frame callbacks then (`ui_visible`).
- **Sound and pacing:** sound goes to ALSA's default, PipeWire's media
  output, so the volume keys set it. A timer runs one frame per tick at
  the core's rate. The tick is nudged by up to 0.5% to keep about 40 ms of
  sound queued, so the sound neither runs dry nor piles up, with no
  resampling. The stream asks for 10 ms periods, because PipeWire sizes
  the graph's quantum, and so the whole path's latency, from them. With
  ALSA's defaults the sound lagged noticeably.
- **Threads:** measured with `S22_EMU_STATS=1` on SMB3.
  - Emulation takes 10.5 ms of each 16.7 ms frame; scaling and drawing
    take 4.5 ms.
  - On one thread that was 91% of a core, and busy moments dropped
    frames.
  - The game now runs in its own thread, on another core. Frames are
    triple-buffered between the two threads. Only one thread is ever in
    the core: the menu's work waits for the game thread to stop.
  - The app also draws opaque (s22ui's `opaque`: no background fill
    first, black only around the picture).
  - Result: a steady 60 fps run and shown, with nothing late.
- **Keys:**

  | Button | Keys |
  |---|---|
  | D-pad | the arrows, or 2 4 6 8 |
  | A (on the right) | 3, 9 or OK |
  | B | 1, 7 or C |
  | Start | # |
  | Select | * |
  | Menu | Back |

  So the game can be held either way: one thumb on the arrows and the
  other on 1 and 3, or everything on the number pad.

  Those are the defaults; the keypad is awkward for games, so the menu's
  Controls page remaps them, per system (the setting `emu_keys_gb` or
  `emu_keys_nes`).
  - Each button takes up to three keys.
  - A key given to one button leaves the one it was on.
  - Back stays the menu. The volume and side keys stay the compositor's.
  - Turbo A and Turbo B, with no keys by default, press their button 15
    times a second while held.

## D049 Notifications and quick settings on the lock screen; folders (2026-10-08)

The lock (D044) showed nothing but itself. Now the side key works on
the lock screen as it does unlocked: notifications first, then quick
settings. New notifications' banners show over the lock too.

- **Placement:** s22-wm moves s22-shell's layer surfaces (namespace
  `s22-shell`) into a scene tree above the lock tree while locked, and
  back at the unlock. No other client's surface goes there.
- **Keys:** while one of the shell's surfaces takes keys, it has them.
  When it closes, the lock gets them back. So do Home and Recents, and
  End held, which closes the list and reaches the lock's own power menu
  (no lk2nd).
- **Opening:** a notification's app never opens over the lock.
  - Choosing a notification there returns to the PIN pad.
  - s22-shell opens it at the unlock (s22-wm says "unlocked") if that
    comes within two minutes.
  - s22-wm says "locked" when the lock starts, and the list closes. The
    shell also asks (`locked` on s22-wm's control socket) whenever it
    opens the list.
- **Quick settings:** most tiles work while locked, the same as
  Android's defaults. Cellular, mobile data and Mullvad VPN need the
  unlock. They show a padlock and say "Unlock to switch this", so
  whoever holds the phone cannot take it off the network or out of the
  VPN without the PIN. Wi-Fi, Bluetooth, sound mode, flashlight and
  brightness stay open.
- **Notification text:** shown in full on the lock screen, as asked.

The home screen gains folders: apps with `Folder=NAME` in their .app file
share one tile, at the place of the first of them. The tile shows their
first four tiles in small. It opens to a grid of its own; Back closes it,
and so does coming back to the home screen. Game Boy and NES are in Games.

## D050 The browser: WebKitGTK in our own shell, a private journal stand-in (2026-10-08)

The user wants a light browser with touch (pinch, inertial scrolling) and
ad blocking, since the screen has no room for ads.

- **Engine:** WebKitGTK 6.0, the engine GNOME Web uses. Its touch is
  native: drag to scroll with inertia, pinch to zoom. It has spatial
  navigation, so the arrows move between links. Its content blocker
  takes compiled rule lists.
  - Firefox was the other choice. It needs no libsystemd and has uBlock
    Origin. It is much heavier for 2 GB of RAM and a 1.2 GHz CPU, and its
    keys cannot be fitted to the keypad.
  - NetSurf is light but runs almost no JavaScript.
- **s22-browser** (C, GTK4) is the shell: a one-line bar (host and title,
  a thin progress line), the page, and a menu.
  - It sends a phone's user agent, so sites send their mobile layouts;
    those fit the 480 px width as it is.
  - Its app_id makes the keypad raw for browsing: 2/8 scroll, 4/6 back
    and forward, 1/3 text size, # address, * menu, 7 and 9 bookmarks.
  - A page script reports when a text field has the focus. The browser
    then writes "text" to s22-t9d's mode file, so T9 types there; Back
    (Esc) leaves the field.
  - Pages may not use the camera, microphone, location or
    notifications. Autoplay is off. Downloads go to ~/Downloads and post
    a notification.
  - Other apps' http and https links come through s22ui's link socket.
- **Ad blocking:** EasyList itself (easylist.to), fetched weekly and
  converted on the phone by `s22-browser-filters` to WebKit's rule format.
  On 2026-10-08 that came to 60,498 rules: 54,460 blocking rules, element
  hiding for 23,811 page elements, and 583 exceptions. Anything WebKit
  cannot express exactly is skipped rather than approximated, so the
  list never blocks more than intended. The ready-made WebKit-format
  EasyList (eyeo's) is no longer updated (last change May 2025), so it
  is not used.
  - Compiling takes a few seconds on the phone and gives a 16 MB rule
    list; later starts load it compiled.
  - Blocking is in the browser only: no DNS or network changes.
- **No systemd:** ALARM's WebKit links libsystemd, for two functions that
  send its log lines to the journal (`sd_journal_send`,
  `sd_journal_send_with_location`); nothing else of it is used. So:
  - s22-browser ships a stand-in `libsystemd.so.0` with just those two,
    which drop the lines. It lives in `/usr/lib/s22-browser`, and only
    the browser's processes find it, through the wrapper's
    LD_LIBRARY_PATH; this includes WebKit's sandboxed page processes.
    No other program sees it, and no systemd package is installed.
  - WebKit and GTK4 are installed with the systemd pieces assumed, as
    PipeWire is (D018): systemd-libs, plus the desktop portals that
    GTK4 asks for and that need systemd. GTK works without the portals.
  - webkitgtk-6.0 and gtk4 are pinned in IgnorePkg, since a plain
    upgrade would stop at those dependencies. They are upgraded on
    purpose, with the same `--assume-installed` flags, when
    held-status shows them behind. That matters for a browser engine.
- **Memory:** one tab, about 520 MB across WebKit's three processes
  (browser 185 MB, network 110 MB, page 220 MB); about 800 MB stays
  free. The first start after a list update briefly takes about 170 MB
  more while the list compiles.

## D051 Every symbol under *, a sliding T9 indicator, tabs in the Shell (2026-10-09)

- **Symbols:** the * key now cycles through all 32 ASCII symbols. It
  used to have 21 of them; the other 11 were only on 1, which keeps its
  short list of common punctuation. The order depends on what is being
  typed, most used first:
  - shell (the console, the Shell app): `/ - . _ | ~ * $ > & ; : " ' = <
    # ! ? , @ % ^ + ( ) [ ] { } \ ``
  - prose (Messages and our other apps, other windows, fields on web
    pages): `. , ? ! ' " - : ; ( ) @ & * / # $ % + = _ ~ ^ | \ < > [ ] { } ``
  - web addresses (the browser's address bar): `. / : - _ ? = & % # @ ~ +
    , ; ! * ' " $ ( ) [ ] { } < > | ^ \ ``

  Each order stays fixed, so a symbol is always the same number of taps
  away in the same place. Reordering by recent use was considered and left
  out, since it moves symbols under the thumb. s22-wm writes "text shell"
  or "text prose" to s22-t9d's mode file for the window in front, and the
  browser writes "text url" while its address bar is out. s22ui's own
  entries use the prose order.
- **The status bar's indicator** gets the whole bar while a key is being
  tapped. The clock and battery give way for those few seconds. A list too
  long even for that slides along, so the chosen character is always in
  view, with ‹ and › where more are hidden. Before, the list stopped at the
  clock: past the 15th symbol nothing showed, which looked like the symbols
  were missing.
- **Ctrl chords on any key:** Recents (Ctrl for the next character) now
  also applies to the D-pad and the other keys s22-t9d passes through.
  Recents then Left is Ctrl+Left.
- **Tabs in the Shell:** foot runs tmux (`/usr/share/s22/tmux.conf`), whose
  status line at the top is a row of tabs under the system status bar,
  in the GUI's theme. Recents then Right or Left switches tabs, Recents
  then Up opens one (in the same folder), and `exit` closes one. A tap on
  a tab switches to it; a tap on the + opens one; a drag scrolls back.
  The session is `new-session -A`: the Shell is kept across Home, and if
  it closes otherwise, it comes back to the same tabs. A user tmux.conf is
  read last.
  - ALARM's tmux links libsystemd. tmux is built from Artix's PKGBUILD
    instead (`--disable-systemd`), pinned in `ARTIX_BUILD` like the other
    Artix rebuilds (D017).

## D052 A clipboard that outlives apps; screenshots; a Help app (2026-10-09)

- **Clipboard:** s22-wm had the standard Wayland clipboard, but a copy
  only lives as long as the app that made it, and Home closes apps. Now:
  - s22-wm reads a new copy's text at once (up to 1 MiB). When the app
    that copied goes away, s22-wm offers the text itself. A paste of it
    is written by a short-lived child, so a slow reader never holds up
    the compositor.
  - The data-control protocols (wlr and ext) let `wl-copy` and `wl-paste`
    (wl-clipboard) use it from commands.
- **The Shell copies and pastes** through tmux:
  - Recents, Down: select mode. The arrows move, 0 starts the selection,
    OK copies, Back leaves.
  - A long press and drag selects with a finger.
  - Recents, V pastes.
  - Copies go to the clipboard through `copy-command wl-copy`; a paste
    loads `wl-paste` into a buffer first. foot's own selections go to the
    clipboard too, not the primary selection.
  - Copy and paste in our own apps (Messages, ...) is still to come.
- **Screenshots:** End + Volume Down runs `s22-screenshot`. It saves
  `~/Pictures/screenshots/Screenshot_DATE-TIME.jpg` (JPEG, so Photos shows
  it) and posts a notification that opens it. The combination cancels
  End's power menu and is ignored during a call.
  - The first screenshots looked crunchy. grim's JPEGs keep colour at
    half resolution, so coloured text and icon edges went ragged, so grim
    now captures raw and cjpeg encodes at quality 95 with full colour
    resolution (`-sample 1x1`).
  - Photos also shrank the 480x640 picture to its 614-pixel view by
    nearest neighbour, dropping a pixel row and column every 25 or so. It
    now blends 2x2 pixels when shrinking (nearest is kept when zoomed in,
    so pixels stay sharp), and decodes the full view with libjpeg's
    accurate DCT.
- **Help** (s22-help): a topic per app and for the phone's keys, typing,
  notifications, the lock screen and the closed lid. Each page is a text
  file in `/usr/share/s22/help` (headings, key rows, bullets), so pages
  are updated without code. `help:NAME` links open a topic.
- Found while writing Help: with the lid shut, one side-key hold both
  switches the flashlight (s22-wm, 500 ms) and dismisses the outer
  screen's notification (s22-outerd, 600 ms). The Help page says so; to
  be resolved.

## D053 Video in the browser; sshd restarts after an openssh upgrade (2026-10-09)

- **Video:** WebKit plays media through GStreamer, but only its core
  libraries were installed, so YouTube would not play. Now the plugin sets
  are installed too: gst-plugins-base, -good, -bad and gst-libav (50
  packages, 27 MB). None of them links libsystemd; the install assumes
  the same systemd pieces as WebKit (D050). They bring:
  - MP4 and WebM demuxers;
  - Venus's hardware H.264 decoder through V4L2 (`v4l2h264dec`), and
    software VP9;
  - output through PipeWire's PulseAudio server.
- **The phone's package lists had gone stale:** a package the plugins
  needed was gone from the mirror. A full `pacman -Syu` came first. It
  upgraded 5 packages, refused udev's replacement by systemd (the pins
  doing their job), and left everything pinned alone.
- **One of those 5 was openssh, and it locked us out.** The running sshd
  starts the new per-connection helper, which no longer matched it, so
  every login was reset at the handshake until sshd restarted (a reboot
  did it here). openssh's own hook only marks sshd for a systemd restart.
  `90-benctix-cat-sshd-restart.hook` now restarts sshd after any openssh
  upgrade, if its config checks out (`sshd -t`). Sessions already open
  stay up, since only the listener restarts.
- Photos' crunchy screenshots had a third cause, the main one: it always
  decoded the full view at 1/4 size, which suits the camera's 1944x2592
  photos, but turned a 480x640 screenshot into 120x160 blown back up. It
  now decodes at the smallest of 1/8, 1/4, 1/2 or full size that covers
  what the screen shows at the current zoom.

## D054 uBlock's filters, YouTube's ads, and media with the lid closed (2026-10-09)

- **uBlock Origin's own list** ("uBlock filters", uAssets, GPLv3) is now
  fetched weekly beside EasyList. Both are converted into one rule set
  (`adblock.json`, 62,808 rules on 2026-10-09), all exceptions after all
  blocks, since WebKit's exceptions only cancel the rules before them.
  - uBlock's `!#if` blocks are kept only where their condition holds for
    WebKit's content blocker, which is Safari's: env_safari and env_mobile
    are true, everything else false.
  - Its scriptlet rules (`+js(...)`) cannot be expressed in WebKit's
    format and are skipped like other unsupported rules.
- **YouTube's ads** come inside its video data, from its own servers, so
  request rules cannot touch them. uBlock's YouTube rules use 19 kinds of
  scriptlets, many of them response rewriters; reimplementing those was
  judged too big and fragile. Instead, `youtube.js`, injected into
  youtube.com at document start while ad blocking is on, does what most
  of those rules do:
  - removes the ad entries (adPlacements, adSlots, playerAds) wherever the
    page parses JSON, reads a fetch answer, or sets its initial player
    data;
  - fast-forwards and mutes any ad that still shows, and presses Skip;
  - hides the ad slots in the page.

  It does not update itself, so when YouTube changes, this file is where
  to follow it.
- **The volume keys with the lid closed did nothing:** s22-outerd's media
  hook looked for PipeWire under /run/user, where it was before D040 moved
  it into the GUI's runtime directory. The hook now uses `/run/s22-gui` as
  the user who owns it, and steps the same 15-level media volume as
  s22-wm's keys, so the two always agree. s22-outerd now only acts on the
  volume keys with the lid closed; with the lid open they are s22-wm's.
  Before, both acted, which went unnoticed only because the hook was
  broken.
- **Now playing:** WebKit publishes its media over MPRIS. A `playerctl
  --follow` in the GUI's audio session keeps `/run/s22-gui/nowplaying`
  ("STATUS TAB ARTIST TAB TITLE"). The outer screen shows it in place of
  the date (♫, title and artist, scrolling when long, grey when paused).
  A long press on a volume key is the next or previous track.
  - The session script now ends its whole process group when stopped, so
    nothing it started outlives it (the D047 lesson).

## D055 Music: mpv as a background player, s22-music as its remote (2026-10-09)

- **Playback is mpv's** (`s22-musicd`: mpv idle, no video, `--no-config`,
  its JSON IPC socket at `$XDG_RUNTIME_DIR/s22-music.sock`). Through ffmpeg
  it plays nearly any audio format, and it reads .m3u, .m3u8 and .pls
  itself.
  - It runs on its own, not inside the app, because Home closes apps:
    the music keeps going.
  - The app starts it when it opens (if it isn't running) and talks to it
    over the socket.
  - The mpv-mpris plugin publishes it over MPRIS. The outer screen's now
    playing, its volume keys' track skip and the side-key double tap
    (play/pause) reach it with nothing further, as they reach the
    browser's videos.
- **s22-music** (C, s22ui) is the remote:
  - Folders: `~/Music` as it is on disk; OK on a song plays its folder
    from it.
  - Playlists: ours, as .m3u8 in `~/Music/Playlists` with paths relative
    to it, so they move with the folder; others found in `~/Music` play
    too.
  - Queue: mpv's playlist.
  - A now playing screen: seek, previous/next, shuffle
    (`playlist-shuffle`), repeat all or one.
  - `*` offers play next, add to the queue, add to a playlist, shuffle a
    folder, and moving or removing songs in a playlist.
  - It reads mpv's state by observing properties, with a small JSON
    parser of its own.
- No tag database: browsing is by folder, as music is usually laid out.
  An Artists/Albums view would need a tag scan (ffprobe over the
  collection) and a cache; left for when it is wanted.
- **The side key twice** (lid closed, s22-outerd) is play/pause, through
  the media hook's `play-pause` (playerctl). A single tap now waits 350 ms
  to see whether a second follows. It remembers whether the screen was lit
  at the first tap, so what a single tap does is unchanged.
- Tested on the phone with a silent 3-second clip: playback over the
  socket, the title, artist, duration and state read back, MPRIS seen by
  playerctl, and the outer screen's now-playing file updated.

## D056 File sharing over SMB; one shared libsystemd stand-in (2026-10-09)

- **What is shared:** the GUI user's home folder, as one share, `home`,
  over SMB (Samba's smbd, SMB 2.1 and later, encrypted where the client
  supports it). Settings > Network & internet > File sharing switches it.
  The service starts switched off.
- **Who may connect:** only micro, with the phone's login password. SMB
  keeps its own copy of a password, in its own format, so Settings asks
  for the login password once. `s22-fileshare`, a root helper that group
  wheel may sudo (like s22-power, D043), checks it against micro's own
  hash in /etc/shadow (libxcrypt) and only then sets Samba's copy. A wrong
  one costs two seconds. The helper acts only for micro, through sudo.
- **From where:** home and office networks only. `hosts allow` takes
  private IPv4, the USB link, and IPv6 link-local and private addresses;
  everything else is denied before any login. That rules out cellular,
  the tailnet and the internet.
  - Addresses rather than interfaces, since smbd binds named interfaces
    once, at start, which at boot can come before Wi-Fi has an address.
- **On and off** is runlevel membership: the helper adds `s22-fileshare`
  to the default runlevel and starts it, or stops it and drops it, so the
  choice holds across restarts. No NetBIOS name service; computers
  connect by address, which Settings shows (`\\ADDRESS\home`,
  `smb://ADDRESS/home`).
- **Samba links libsystemd** for `sd_notify`, `sd_notifyf` and journal
  logging, all pointless here. WebKit's stand-in (D050) became its own
  package:
  - `s22-standin` puts `libsystemd.so.0` with those four do-nothing calls
    in /usr/lib/s22-standin, outside the library path.
  - The browser and the file sharing service put it on their own
    LD_LIBRARY_PATH; nothing else sees it.
  - samba, smbclient and libwbclient are pinned: a newer Samba could call
    something the stand-in lacks, so it is upgraded on purpose.
- Tested on the phone: smbd started on the stand-in and listened on port
  445 (IPv4 and IPv6), then stopped. The helper's status works through
  sudo as micro, and it refused a wrong password after two seconds. The
  first real login is the user's, from their computer.

## D057 Browser tabs: five at most, put away when memory runs short (2026-10-09)

- **Up to five tabs**, each a WebKitWebView in a GtkStack.
  - Their numbers show as small buttons at the bar's right, only with two
    or more, so one page keeps the whole screen. A tap switches, `+`
    opens one; the `*` menu lists them too.
  - Keys (browsing mode is raw, and Recents is the compositor's there, so
    the Shell's Ctrl chords are out): a hold on keys already in use, so a
    tap keeps its meaning.
    - 4 / 6 held: the tab before or after.
    - 5 held: a new tab.
    - C held: close this tab; the last one closes the browser.

    Those four keys now act on release (or when the hold is reached).
  - Links asking for a new window, and links from other apps, open a new
    tab while there is room.
- **Restored lazily:** all tabs come back next time ("@N" then a line a
  tab), but only the one in front loads; the others load when first
  shown.
- **Memory:** related views do not mean one web process. WebKit gives
  each site its own: two tabs (YouTube and a small page) came to about
  850 MB across the browser's processes.
  - Below 400 MB available, the tab out of view longest has its web
    process ended (never one showing the same site as the front tab,
    which may share its process), keeping its address. It loads again
    when shown.
  - Any tab whose process ends, for whatever reason, reloads: at once if
    in front, else when next shown. Tested by killing the front tab's
    process: a new one came up and the page reloaded.

## D058 Music: one command for a whole list, a socket that never blocks, a Songs tab (2026-10-10)

- **The app hung on a long playlist.** s22-music sent mpv one `loadfile`
  a song, all at once, without reading mpv's answers between them. With a
  687-song playlist (MediaMonkey's, synced over SMB) both directions of
  the socket filled after about 280: the app blocked for good in
  `write()`, and mpv's IPC thread spun on EAGAIN, a whole core at 100%.
  The songs already sent played, so the playlist seemed to work, but the
  app took no key after it: no play/pause, no other song. The outer
  screen's keys still worked, being MPRIS, not the socket.
  - Songs now go to mpv as a list file in `$XDG_RUNTIME_DIR` (four used
    in turn) and one `loadlist` (replace, append, append-play or
    insert-next), whatever their number; 687 load in 0.05 s. Paused while
    loading, so the first song does not sound on the way to the one
    chosen; shuffle is done before playing too.
  - The socket is non-blocking: what does not fit waits in a buffer, sent
    as mpv reads (after each read from it, and on a 50 ms timer).
  - The queue is asked for once per batch of mpv's events, not once per
    playlist-count change (each answer is the whole queue). The read
    buffer is 1 MiB, a queue of 4000 songs.
- **Songs**, a tab between Folders and Playlists: every song in ~/Music,
  however deep (eight folders), by file name less a leading two-digit
  track number ("06 Artist - Title" as MediaMonkey names them; all 687
  on the phone are so), sorted naturally, which groups them by artist.
  Read again each time the tab opens. OK plays them all from that song;
  `*` offers play, play next, add to the queue, add to a playlist and
  shuffle all. No tags read, as in D055.
- **4 and 6 page** up and down in every list (one screen), as they seek
  in now playing.
- **Playlists from Windows**: a UTF-8 byte order mark before the first
  line is skipped (it had made the first song's path wrong); backslashes
  become slashes, and a drive letter path (the share mapped as a drive,
  whose root is the home folder) is read under ~.
- **A player bar** replaces the one-line mini player (40 px, 76 now)
  while a song is loaded: the song's progress as a bar along its top,
  playing or paused, title, artist and "time / length". A tap opens now
  playing, as `#` does; **5** plays or pauses from any list. The bar's
  clock runs while playing (the time asked for each second; at once when
  playing resumes).
- **Now playing's clock stood still** when the screen was reached by OK
  on a song: only `#` and the bar started its timer. Every way there now
  goes through one function that does, and Back returns to the list it
  came from (it went to Folders).
- **Left / Right held scrub** in now playing: 10 s a second, 30 after two
  seconds, sought every 250 ms and once more on release; a tap is still
  the song before or after, now on release. Timed by the app, not by key
  repeats; losing the keyboard ends it.
- **Album art** (MediaMonkey wrote an albumart.jpg into each of the 241
  album folders, and 39 of 40 songs sampled carry the cover too; the app
  showed none):
  - Found as: the song folder's albumart.jpg, cover.jpg, folder.jpg or
    front.jpg, else any JPEG or PNG there, else the song's own picture.
    A JPEG is read and decoded at once (libjpeg, at 1/2 to 1/8 when big;
    the decoder is Photos'). A PNG or the song's picture goes through
    ffmpeg into a JPEG in the background (1.6 s for a FLAC here), so the
    app never waits on it.
  - Now playing: the cover fills the space from the header to the
    progress bar, cropped to fit, behind the title. It fades into the
    theme's background: mostly (78%) behind the title, artist and album,
    their height measured first, easing off over 90 px below them, and
    into the background again over the bottom 40%. Readable in the dark
    and light themes (checked by rendering both on the phone).
  - The bar: a 64 px cover at its left, dimmed under ❚❚ while paused.
  - No cover: a record, drawn (grooves, a label in the accent colour).

## D059 Music everywhere: the notification list and the lock screen (2026-10-10)

The Music player becomes part of the system, not only its app.

- **One module for it, s22np.c in libs22ui** (linked with -ljpeg by
  Music, s22-shell and s22-lock):
  - follows the player (s22-musicd's mpv) on its socket, each follower
    with its own connection (mpv takes several), made when the socket
    appears (inotify on the runtime directory: no polling while nothing
    plays); the time is asked each second only while a view shows it
    and the song plays;
  - Left/Right: a tap the song before or after, held scrubbing (as in the
    app, D058), timed by the module;
  - the covers, moved out of the app (D058's rules), with the backdrop's
    fades now given by each view;
  - the record, and **the card**: cover, title, artist, before / play or
    pause / after (drawn shapes: DejaVu has no ⏮ ⏭), the times, and a bar
    along its foot. Taps on the buttons and the bar.
  MPRIS was the other way (it covers the browser's videos too), but
  mpv-mpris hands the cover over as a data: URI of the whole picture, and
  its position read 0 while playing; the socket has it all.
- **Notification list:** the card heads it while a song is loaded, chosen
  when the list opens. OK plays or pauses, Left/Right as above, Down to
  the notifications, Up from the first one back to it. Over the lock too
  (D049).
- **Lock screen:** the cover fills the background (63% background behind
  the clock and date, 35% around the card, 84% behind the PIN) and the
  card shows under the date, 78% opaque. Left/Right as above; OK plays or
  pauses until a PIN digit is typed, then unlocks as before. Taps on the
  card. The PIN works as it did.
- **Now playing in the app:** the cover's top is no longer cut off: it is
  kept from the top, under the header, cropped at the bottom. It reaches
  down to the hints, behind the controls, blended only 65% there (it
  faded to the background before), and 59% behind the title (was 78%),
  clear between. The times there are in the text colour, over the cover.
- Checked by rendering all three views from the built library on the
  phone, with a real cover.
- **Text over a cover gets a halo** (ui_text_box_halo, ui_text_wrap_halo):
  the text drawn into a mask, box-blurred twice each way (radius 5, near
  a gaussian), and the picture under it blended up to 84% toward the
  theme's background, before the text. The fades alone left text hard to
  read over busy or bright covers; now the title, artist, album, times
  and controls in Now playing, and the clock, date and PIN prompt on the
  lock screen read over any cover, while the rest of the cover stays as
  it is. The date, album, times and "Enter PIN" are in the text colour
  over a cover, not dimmed.
- **The slider on a panel of its own:** in Now playing the progress bar
  and its times sit on a rounded panel of the background, 80% opaque
  (ui_round_alpha), so they stand out over any cover; a sticker on one
  cover had covered the time. The times there need no halo.

## D060 The loudspeaker in mono; songs evened in loudness (2026-10-10)

- **Mono:** the phone has one loudspeaker, so "Media" (D040) is a mono
  sink: apps' stereo is mixed down to it by PipeWire, (L+R)/2, and the
  mono goes to both of the amplifier's I2S channels. Nothing panned to
  one side is lost. Done in software rather than in the AW88194A's own
  channel select, which stock's profile sets and which is not decoded
  in our notes. Checked silently (Media's way to the speaker muted for
  the test): a tone in the right channel only came out of Media at half
  its level (-18 dB in, -24 dB out). Media only ever plays into the
  loudspeaker; a stereo output (Bluetooth, the headset) would need its
  own way in. Rings and notifications (s22-play, straight to the
  loudspeaker) are as they were.
- **Loudness:** no song of 60 sampled carries ReplayGain or R128 tags,
  so the Music player (s22-musicd) runs ffmpeg's dynamic normalizer:
  `dynaudnorm=f=500:g=31:p=0.95:m=10:r=0.2` (500 ms frames smoothed over
  31, about 15 s, so it follows songs rather than beats; RMS 0.2, peaks
  under 0.95, at most 20 dB up). Five songs measured over 45 s: 8.2 dB
  between the loudest and the quietest as they are, 2.1 dB through it.
  About 3% of a core; loudnorm (EBU R128) took 36%. Scanning each song
  once for a gain was the other way (4.7 s a song, about 33 min for the
  library, and kept up as songs come); the filter needs nothing kept.
  ReplayGain tags still apply first where a song has them.
- **Restarting PipeWire needs the player restarted too.** After
  `rc-service s22-audio restart`, mpv's `ao-reload` answered success but
  its PipeWire output never came back: every song then failed to open
  its output and mpv went straight to the next, through the whole queue
  in seconds, paused or not. A fresh mpv opens it at once. So after a
  PipeWire restart, s22-musicd is stopped and started again (the queue
  can be saved over the socket and loaded back with loadlist); the app,
  the notification list and the lock screen reconnect by themselves.
- **C stops** (Now playing in Music, and the player's card in the
  notification list, where C dismisses): mpv's `stop keep-playlist`. The
  player goes idle, so the bar, the card and the lock screen's player go
  too; the queue stays, and OK on a song in Queue plays again. On the
  lock screen C stays the PIN's.
- The Music and Sharing help pages had `\t` written out instead of tabs
  since D055/D056, so Help showed their key tables as text; now tabs.

## D061 Doom (UZDoom) on the SD card; GPU clients' frames waited for (2026-10-10)

The user asked for UZDoom, on the SD card, without mouselook by default.

- **Built here, not on the phone:** no aarch64 build exists (ALARM has no
  UZDoom; its releases are x86_64 and macOS), and the phone would take
  hours. `tools/build-uzdoom.sh` cross-compiles 5.0.3 against the phone's
  own /usr (rsync over USB): it links the phone's glibc 2.43 (`-B`/`-L`,
  not the toolchain's 2.44), keeps the phone's headers after the
  toolchain's (`-idirafter`, so libstdc++'s wrappers work), and builds
  UZDoom's tools (zipdir, re2c, lemon) natively first. Vulkan is off (no
  driver for the Adreno 308). The package, `uzdoom-s22`, goes on the card
  with `pacman-sd -U`; its tile ("Doom", Games folder) starts `s22-doom`.
  pacman-sd now takes a tile's folder, icon and colour from `X-S22-Folder`,
  `X-S22-Icon` and `X-S22-Color` in the .desktop entry.
- **The keypad's game mode:** SDL has no scancodes for OK, Call, * or #
  (`KEY_OK`, `KEY_PHONE`, `KEY_NUMERIC_STAR`/`POUND`), so in raw mode a game
  never sees them. A window whose app ID starts `game-` now puts s22-t9d in
  "game" mode: keys pass through, the keypad's own as PC keys (OK Enter,
  Back Escape, Call Left Ctrl, * keypad *, # Tab, Speaker Left Alt).
  UZDoom names its window through `SDL_VIDEO_X11_WMCLASS` unless it is set
  already, so `s22-doom` sets that to `game-doom`.
- **First settings** (copied once, then UZDoom's own): IWADs and mods from
  `~/Games/doom`; no mouselook and no mouse (a finger on the screen would
  turn the view); always run; vsync; the frame rate in the corner; keypad
  bindings (help page Games). The GLES backend in an ES context (the GPU has
  OpenGL 3.1, ES 3.0).
- **freedreno's a3xx depth clamp does not clamp:** with `GL_EXT_depth_clamp`
  on (UZDoom enables it for the session), sector floors and ceilings that
  reach behind the camera were drawn over the walls, the same in every
  frame. llvmpipe drew the scene right; with depth clamp off the GPU does
  too, at 60-80 frames a second. `a3xx-depth-clamp.patch` turns it off on
  FD3xx renderers. Not the cause, tried on the way: the GLES and desktop GL
  paths, client-side indices, a 10x smaller vertex buffer, light and bone
  uniform sizes, no binning, no blitter, the shader compiler's switches.
  `FD_MESA_DEBUG=sysmem` hangs the GPU, and the kernel then fails to bring
  it back ("gpu hw init failed: -22", TODO): only a reboot recovers.
- **s22-wm waits for a GPU client's frame before taking it**, as GNOME
  (Mutter) and KDE (KWin) do; wlroots relies on implicit sync instead. A
  harness (the game started from its title, a virtual keyboard through the
  menu and moving, grim sampling about 85 frames) found 10-39 black frames
  without the wait, whatever the game's renderer (GL or software) or frame
  cap, and 0 with it (only the frames UZDoom draws black while a level
  loads). About two thirds of the game's commits arrive while its GPU is
  still drawing. On commit s22-wm exports the buffer's pending writes
  (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`) and holds the commit
  (`wlr_surface_lock_pending`) until they are done; the game draws its next
  frame meanwhile. `glFinish` in the game also cured it, at 40 instead of 60
  frames a second. Not the cause, measured: the scheduler's dependencies
  were in place (`drm_sched_job_add_dep` traced) and the game's fence had
  already signalled when the compositor's job was checked, so a kernel
  change making a3xx jobs wait for the full fence
  (`DRM_SCHED_FENCE_DONT_PIPELINE`) changed nothing and was dropped (built
  as 7.1.3-34 for the test; the phone boots 7.1.3-33 again). Mesa's
  `disable_explicit_sync_heuristic` (Mesa sets it for Xwayland) made no
  difference set for the game alone; the GUI session sets it for s22-wm,
  whose own frames go to the display, but its effect there is unproven.
- **Still open:** on the panel the game still flickers (textures, old
  frames) while grim's captures of the same moments look right, so it is
  after composition (s22-wm's frames to the display) or not visible to
  screencopy. Parked at the user's word; Doom stays on the GPU renderer.
- The display's "Atomic commit failed: Device or resource busy" came almost
  only while grim captured frames (1 in 15 s of play without it).
- **Screen timeout "Never" did not take:** s22-wm rereads the settings
  file when its time changes, compared in seconds, and stepping through
  the choices wrote it twice in one second. Now to the nanosecond.

## D062 Bluetooth audio: music to a speaker, in stereo; paused when it drops (2026-10-10)

The Music app (and the browser) did not see a connected Bluetooth speaker
(an Archeer A320); A2DP had worked once (D018), before the sound server
moved into the s22-audio service (D040).

- **WirePlumber never ran its Bluetooth monitor:** it does only while the
  user's login seat is "active" (seat monitoring, with elogind present),
  and the GUI's user runs as a service with no login session. So the phone
  offered no A2DP source; a speaker connected, found nothing to play and
  left. `51-s22-bluetooth.conf` turns seat monitoring off. With it the
  adapter lists "Audio Source" again.
- **Music out only** (`bluez5.roles = [ a2dp_source ]`): calls stay on the
  modem's audio path, so the phone is never a headset gateway (HFP/HSP).
  SBC-XQ is allowed. PipeWire picks the best codec both sides have (LDAC,
  aptX, AAC above SBC); it ranks SBC-XQ just under plain SBC, so for the
  A320 (SBC and SBC-XQ only) the XQ profile was chosen once by hand
  (`pactl set-card-profile ... a2dp-sink-sbc_xq`), which WirePlumber
  remembers per device. Stereo, 48 kHz.
- **A Bluetooth output is the default while it is there** (priority 3000,
  above Media's 2000): the music player, the browser and the volume keys
  (`@DEFAULT_AUDIO_SINK@`) follow it, and fall back to Media when it goes.
  Media stays mono (D060); the Bluetooth path is stereo.
- **A drop never reaches the loudspeaker:** s22-audio-session watches the
  sinks (`pactl subscribe`). While a Bluetooth output is there, Media is
  muted, so whatever WirePlumber moves back to the loudspeaker arrives
  silent; when it goes, every player (MPRIS: mpv, the browser) is paused
  and Media unmuted a second later. Seen working: switching the A320's
  profile removed its output for a moment, and the music paused.
- **The media volume goes to the device as it connects** (AVRCP), the
  level the volume keys keep.
- **WiFi starves it:** with WiFi on, the speaker's answer to the stream's
  start (AVDTP Start) came after BlueZ's timeout ("Connection timed out",
  then "Transaction label doesn't match"), every 30 s; with WiFi off the
  stream starts and plays. The shared radio's coexistence (D019) again.
  Open (TODO).

## D063 Doom's stale frames: each frame finished before the swap (2026-10-11)

D061 left UZDoom flickering on the panel: textures, and old frames shown
between new ones. Fingerprinting every buffer s22-wm took found UZDoom
rotating three window buffers with only one fresh at a time (305 frames
in 15 s of turning went back to the one before last); es2gears and Luanti
(Minetest, two buffers, drawn straight to the window) never did.

- **`s22-glfinish.patch`: `glFinish` before every `SDL_GL_SwapWindow`.**
  The same harness counted 0 stale frames with it, still with three
  buffers: the buffer count was not the cause. A frame handed over while
  the GPU still draws it is shown with its old contents, s22-wm's wait for
  the buffer's fence (D061) notwithstanding. `glFlush` instead (submitted,
  not waited for) brought the flicker back, so it is not a submit left in
  Mesa: why the exported fence does not cover the frame is open (TODO).
  The cost is the overlap of CPU and GPU: about a fifth of the frames.
- **Settings for the GPU** (first settings, `uzdoom.ini`): textures
  unfiltered (trilinear and 16x anisotropic before; with them off the
  remaining texture flicker went too), no dynamic lights (`lights.pk3` not
  loaded, nor on sprites and particles). Measured standing at Freedoom 2's
  MAP01 start: 41 frames a second; at half resolution 52, so the GPU's time
  goes mostly to what it draws, not to pixels, and the user kept full
  resolution (480x614, the window, drawn 1:1). The software renderers were
  no faster (38 true colour, 43 paletted). The GPU runs at its highest
  clock (400 MHz) throughout; the game's thread uses under half a core.
- **No vsync, no sprite shadows** (the user's choice, no frame cap below
  60): with the game waiting in `glFinish`, vsync made a frame that missed
  a refresh by a little wait for the next one, and the game drop to 30 a
  second. Wayland does not tear. Sprite shadows draw every monster twice.
- **Keys for every game family:** UZDoom keeps bindings per family (Doom,
  Heretic, Hexen, Strife, Chex), and the keypad's were only Doom's, so
  Chex Quest started with the PC defaults (WASD and the mouse) and the
  keypad did nothing. The first settings now give each family the same.
- **The desktop GL renderer** with Mesa told it has GL 3.3 (the Adreno 308
  has 3.1) ran slowly, then hung the GPU, which needs a reboot (TODO): not
  an option.
- Luanti (Minetest) went on the card for the comparison (`pacman-sd -S
  luanti`, Minetest Game in `~/.minetest/games`). Its desktop GL renderer
  wants GL 3.2 / GLSL 1.50; it runs with
  `MESA_GL_VERSION_OVERRIDE=3.2COMPAT MESA_GLSL_VERSION_OVERRIDE=150`
  (its tile since D064).

## D064 Luanti in the Games folder; typing in a game (2026-10-11)

- **`luanti-s22`** (packaging/luanti, arch any, on the card with
  `pacman-sd -U` after `pacman-sd -S luanti`): `s22-luanti` starts Arch's
  Luanti with the GL 3.2 override (D063) and the app ID `game-luanti`
  (`SDL_APP_ID`, through sdl2-compat), so the keypad is in game mode, and
  copies the first settings once: fullscreen, touch controls (drag looks,
  tap places, hold digs), keypad bindings. Luanti binds keys by SDL
  scancode and has look left, right, up and down actions, so the keypad
  alone can play: the D-pad walks and turns, 2 and 8 look up and down, 4
  and 6 strafe, 5 jumps, 0 sneaks, OK digs, Call places, C drops, 1 and 3
  step through the hotbar, # the inventory, * chat, 7 fly, 9 fast, Speaker
  Aux1. The digits' default hotbar slots are unbound. 15-20 frames a
  second in play.
- **pacman-sd: `X-S22-Replaces`.** Our entry stands in for the package's
  own (`org.luanti.luanti.desktop`), so there is one Luanti tile, ours.
- **Recents in a game switches the keypad to typing** (s22-t9d 0.12): a
  saved game's name in Doom, chat in Luanti. Text mode until Recents
  again, or until s22-wm rewrites the keypad's mode (another window,
  noticed by the file's time); a key held across the switch is released
  as it was pressed. Recents switched apps in a game before; Home still
  goes home. The status bar shows the typing mode for a game window too
  (s22-gui 0.66), since its mode stays "game".

## D065 Speech: BENCmouth says who is calling and reads texts (2026-10-11)

The user's own synthesizer, BENCmouth (bropple/BENCmouth), as the phone's
text to speech: who is calling, and texts read aloud, each a switch in
Settings, Accessibility.

- **`bencmouth`** (packaging/bencmouth) builds the v0.3.2 tag on the phone
  in about a minute: `bm` with CMUdict compiled in (`make dict`, 124,910
  words), `libbencmouth.a`, its header, the voice files. It renders 13 s of
  speech in 0.27 s here. `bm` takes about 1 KB of text at a time ("input
  exceeds a compile-time capacity"), and reads text starting with "-" as
  an option unless it starts with a space.
- **`s22-say`** (packaging/s22-say, the s22-say service, root): what is
  sent to `/run/s22-say/sock` (group wheel) is said in order, one thing at
  a time: `bm -v VOICE -o -` into `s22-play -`, so the loudspeaker at the
  ring and notification volume, mixed like any alert. A long text goes in
  pieces of up to 300 characters, cut after sentences. "say TEXT" queues,
  "wait TEXT" answers once it has been said, "stop" silences it and
  empties the queue. `s22-say` (and `say`) is also the client.
- **s22-play plays WAV** (s22-notify 0.8): a 16-bit PCM file, or standard
  input with "-", so speech gets the same device, volume and stop on
  SIGTERM as the tones.
- **Who is calling** (s22-call 0.14, `tts_calls`): "Call from NAME" (the
  contact, else the number digit by digit, which bm would otherwise read
  as one large number), then RING_SPELL (6 s) of the ringtone, over again
  until the call is answered, ends or is silenced; each step waits for the
  last ("wait"), and off() sends "stop". Without s22-say the ringtone
  rings as before. A silent ringtone or sounds off stays silent.
- **Texts read aloud** (s22-notifyd, `tts_messages`): for source "sms",
  1.2 s after the tone, "Message from NAME. TEXT", the number s22-smsd puts
  before the text taken off. Not with sounds off or during a call.
- **Settings, Accessibility** (s22-gui 0.68): the two switches, off to
  start with; the voice (`tts_voice`, default BENCmouth; the presets as
  `bm -l` lists them, each heard as it is chosen); a sample.

## D066 Wallpapers (the outer one moving); the side key with the lid shut; the loudspeaker's lost 20 dB; the PIN unlocks as typed (2026-10-11)

- **Wallpapers** for the home screen, the lock screen and the outer
  screen, or none (as before): pictures in `Wallpapers/inner` and
  `Wallpapers/outer` on the memory card, chosen in Photos' new Wallpapers
  tab, where each shows as it will look (Settings' rows open it for that
  screen: `wallpaper:home`, `:lock`, `:outer`; OK sets, * sets none). A
  photo moves there from the Photos tab (OK). The first try, a plain list
  in Settings, showed nothing of the pictures. `s22-wallpaper` (s22-gui) converts the chosen one once with
  ffmpeg into a binary PPM in /var/lib/s22, so nothing on screen decodes
  pictures: 480x640 for the inner screen (its first frame), 128x128 for
  the outer one at 20 frames a second, up to 20 s, each cropped to fill,
  transparency on black; the choice is kept as `wallpaper_home`,
  `wallpaper_lock`, `wallpaper_outer`. A 7.5 MB, 320-frame 480x480 GIF
  takes 2.8 s and becomes 159 frames (7.8 MB). `ui_wallpaper()` (s22ui)
  fills a canvas by width, bottom-aligned; the launcher's window is the
  whole screen, so a 480x640 picture is drawn 1:1 with the status bar and
  the hints over it (its own title bar, hidden by the status bar but for an
  8 px band, is left out with a wallpaper: it cut off the top); the lock screen
  shows its own unless music's cover is up, and the power menu stays
  plain. s22-outerd reads several PPMs one after another as an animation,
  played from the start whenever it lights, redrawing every 50 ms while it
  is lit (the panel is off with the lid open), at `wallpaper_speed`
  (Settings, Outer screen: a quarter to double); its wallpaper moved from
  /var/lib/s22-outer to /var/lib/s22, which Settings' user may write.
- **The outer screen's date stays** under the clock with what plays under
  it (it had taken the date's line), each about 2 px apart, the date
  shadowed like the clock.
- **The side key with the lid shut** both switched the flashlight (s22-wm,
  held 0.5 s) and dismissed the notification shown (s22-outerd, 0.6 s).
  Now a long press with the lid shut only dismisses, and side + Volume
  down is the flashlight (the user's choice): s22-wm switches it, and
  s22-outerd leaves the chord alone (no volume step, no tap). With the
  lid open a long press is the flashlight as before.
- **Settings' battery voltage** read a supply named "battery"; the fuel
  gauge here is `pm8916-bms-vm`, so it always said 0.00 V.
- **The loudspeaker lost 20 dB at a boot** (since D062): s22-audio-session
  set the media volume on the default output 3 s after it started, and
  then the default could still be the loudspeaker itself, whose volume is
  the amplifier's `Speaker Volume` (1.0 is 192, the maximum; 0.41 is 145):
  volume 8 left it at 145, under the media volume as well. It now sets
  Media, and a Bluetooth device, by name. The loudspeaker was put back to
  1.0 by hand.
- **The PIN unlocks as it is typed**: from the fourth digit the lock
  screen tries it once the dot is drawn (a yescrypt check takes 122 ms
  here) and unlocks when it is right; only OK counts a wrong one, and
  nothing about the PIN (its length) is stored for it.

## D067 Music opens on now playing, at the right time; Home closes a folder (2026-10-11)

- **Music showed a paused song at 0:00** until it played again: the
  position (`time-pos`) was only asked by the app's clock, which runs
  while a song plays or now playing shows. It is now asked as the player
  connects, when it pauses (from anywhere: the lock screen, the outer
  screen's side key) and when the song changes.
- **Music opens on now playing** when a song is loaded, playing or
  paused (the user's idea): the player's first word on what is loaded
  (`path`, observed last, so the rest is known) decides; Back goes to the
  lists as before.
- **Home closes an open folder** on the home screen. The launcher closed
  one when it got the focus back, but in a folder it has the focus
  already, so Home changed nothing. s22-wm now also sends it `home:` on
  its app socket, as links reach any app.

## D068 The outer screen's volume as 0-15, without stalling; text in any script (2026-10-11)

- **The outer screen's volume** showed a percentage of the media volume,
  and the screen (a moving wallpaper too) stood still while its media hook
  ran (setpriv, sed, wpctl; it was waited for). The hook now prints the
  level as the GUI counts it ("Level: 8 15"; a "Volume: 0.53" fraction from
  a replacement hook still works), and s22-outerd shows the new level at
  once (from the settings' "volume", or the last level while more steps are
  on their way) and runs the hook in the background, one press after
  another, so quick presses all count.
- **Text in any script.** The GUI drew with DejaVu alone (Latin, Greek,
  Cyrillic, Arabic), so Japanese song titles were boxes; the outer screen
  had an ASCII bitmap font and showed "?". Noto (`noto-fonts`, 107 MB) and
  Noto CJK (`noto-fonts-cjk`, 299 MB; the root has 8 GB free) are
  installed, and a character DejaVu does not have comes from the font
  fontconfig finds for it, asked once per character and each file opened
  once, in s22ui and in s22-outerd (FreeType at 14 px on the 8x16 font's
  baseline, ASCII still the bitmap font). The pattern names DejaVu Sans,
  then Noto Sans CJK JP, then sans-serif: with `lang=ja` alone fontconfig
  chose the Korean face of the same .ttc, whose shared kanji are drawn the
  Korean way. Programs pick it up as they start again.

## D069 Suspend reaches VDD minimisation (2026-10-11)

Deep sleep only ever worked in idle (D030: 54% of the time in VDD
minimisation, vmin, unplugged with the lid closed); an s2idle suspend
reached it never. A probe (`/root/sleepprobe.sh`, then
`suspendprobe.sh` and `xoprobe.sh`: unplugged, lid closed, RPM stats from
its message RAM, clock and device-PM tracepoints across the suspend)
found four causes, each fixed in BENCkernel-CAT:

- **7.1.3-35, the MPM's wake timer:** our MPM driver tells the RPM when
  to wake the AP from the domains' next timer (D030). In s2idle the
  ticks are frozen and that timer is stale, already past: the RPM woke
  the AP at once, ~490 times a second (146792 MPM interrupts in 300 s).
  No timer is programmed while entering s2idle; wake pins and the RTC
  alarm (through SPMI) wake it.
- **7.1.3-36, clocks left on after every resume:** i2c-qup, qcom-cci
  (the cameras' I2C) and spi-qup (the outer display) enabled their clocks
  in system resume whatever runtime PM thought, and runtime PM, which
  still had them suspended, never disabled them: XO stayed voted after
  the first suspend until the next boot, idle vmin included. All three
  sleep through `pm_runtime_force_suspend/resume` now.
- **7.1.3-38, the USB glue:** ChipIdea's core resumes itself (and the
  msm glue, D030) before it puts the controller in low power, and the
  glue had no system sleep callbacks: its clocks (GPLL0, so XO) stayed on
  through every suspend. The glue gates them for the sleep, in the late
  phase: 7.1.3-37 did it in the normal phase, before its child the core,
  which then touched the registers unclocked (a synchronous external
  abort in `ci_hdrc_enter_lpm_common()`, caught by ramoops, which works
  now).

Result (7.1.3-38): a 300 s s2idle spends ~293 s in vmin (~95%), XO is
released after the resume (idle 56% the next 2 minutes), and USB comes
back. Still missing before the phone suspends by itself: the modem's
SMD interrupt is no wake source (calls and texts wait for another
wake), and the suspend policy (lid closed, nothing playing).

## D070 The phone suspends while its lid is closed (2026-10-12)

With suspend reaching VDD minimisation (D069) and the modem able to wake it
(7.1.3-39: its SMD edge is a wakeup source, and RESIN, volume down, joins
volume up, the side key, the lid and the power key), `s22-sleepd` (in
s22-lidd) suspends the phone while the lid is closed and nothing needs it.

- **When:** every 2 s while awake it looks for a reason to stay up: the
  lid open (`/run/s22-sleep/lid`, from a new s22-lidd hook), USB data
  connected (a computer; a charger alone does not count), a call ringing
  or going on (`/run/s22-call/state`), something playing (paused does not
  count, the user's choice), a sound device open (ringtone, a text's tone,
  speech), the outer screen lit, pacman running, a file in
  `/run/s22-sleep/inhibit/`, or `lid_suspend=off`. With none for 4 s, and
  at least 6 s after the last resume, it suspends; the kernel's
  `wakeup_count` makes that race-free (a wakeup in between aborts it).
  The waits are short because whatever a wake brings holds the phone up
  by itself: a call's state, the tone and the speech, the outer screen.
- **The modem's chatter:** at a weak or fluctuating signal the modem woke
  the phone every few seconds. A kprobe on `qrtr_endpoint_post` (its
  header and the QMI message ID, from the packet) named the messages: NAS
  signal-info indications (0x51), crossing the bar thresholds. s22-celld
  turns them off while the lid is closed and reads the signal after every
  resume (s22-sleepd rewrites `/run/s22-sleep/resumed`) and when the lid
  opens, so the outer screen's bars stay at most a sleep old. What is left
  at a bad signal is the modem's IMS retrying its PDN through s22-imsd and
  serving-system changes, which must be answered; with full service the
  modem was silent for 5 minutes and more.
- **Measured** (lid closed, unplugged, limited service, 15 minutes): asleep
  89% of the time, 18 suspends, ~6 s awake each. A text sent during a
  suspend wakes the phone within seconds (D069's follow-up test). A call
  to a suspended phone is still untested (the network gave only limited
  service at the time).
- WiFi drops at each suspend (no WoWLAN) and reconnects after; mobile data
  stays off (`mobile_data=off`, and s22-celld waits 10 s anyway).

## D071 Sleeping with WiFi up, a cable that wakes it, End for a refused call (2026-10-12)

Follow-ups to the lid-closed suspend (D070):

- **WiFi stays connected through a suspend.** Without WoWLAN mac80211
  disconnected at every suspend and took seconds to come back. s22-sleepd
  now enables WoWLAN on magic packet only before each suspend (the phy can
  be made again): the WCN3610 firmware keeps the association and rekeys
  (GTK offload), answers ARP and IPv6 neighbour solicitations, and wakes
  the phone for nothing else. That first woke it every few seconds: the
  firmware still passed broadcast and multicast up (IPv6 router
  advertisements every 3 s), each an RX interrupt, a wakeup source.
  BENCkernel-CAT 7.1.3-40 sets wcn36xx's `bcast_mcast_filter` (3, both)
  with the host-suspend power parameters, as prima does.
- **A cable plugged in while suspended** was lost twice over. The charger's
  VBUS interrupt is an edge, masked through the suspend (7.1.3-43:
  pm8916_lbc makes it a wakeup source and reads VBUS again on resume; its
  extcon is what tells the USB controller), and the WUSB3801 USB-C
  controller's handler read the chip over I2C while runtime PM refused the
  bus for the system sleep, -EACCES (7.1.3-42: it keeps its interrupt off
  through a sleep and reads the port on resume). Moving i2c-qup's sleep to
  the noirq phase did not help (runtime PM is off from suspend_late to
  resume_early whatever the phase): 7.1.3-41, dropped. s22-sleepd stays
  awake 20 s after the cable comes or goes, so a computer can set up USB
  (a suspend in between left the link dead), and a charger alone then
  lets it sleep. USB data connected keeps it awake as before.
- s22-sleepd logs the device interrupts that came in each suspend (the
  kernel recorded no wakeup IRQ for these wakes), and the WiFi link one
  second after each resume.
- **Settings, Display: "Sleep with the lid closed"** (lid_suspend, on).
- **End for a call the modem refused** (s22-call 0.15): in limited service
  the modem reported call 1 as cc-in-progress, then refused the dial
  (IncompatibleState) and never ended it; s22-calld kept it, and End
  failed (InvalidQosId). It now asks the modem for its calls
  (get_all_call_info) when a dial or a hang-up is refused, and when a dial
  is still dialling after a minute, and ends the ones the modem has not.

Measured (7.1.3-40, lid closed, unplugged): WiFi linked 1 s after every
resume; the WiFi interrupt no longer started any wake.

## D072 WiFi catches up as the lid opens (2026-10-12)

While suspended the phone does not look for WiFi (the user's choice: a
phone in a pocket has nothing to find, and calls and texts do not need
it). Carried out of range it keeps its old association (WoWLAN, D071): the
firmware reports the lost link over its SMD channel (event 312, which
wcn36xx ignores), and that is no wakeup source, so Linux still believes it
is connected until mac80211 misses enough beacons after a wake, and a
modem's short wake can end first.

An s22-lidd hook (`30-wifi-rejoin`) catches up when the lid opens: a scan
of the current channel for the current access point (about 0.2 s), and if
it was not just heard, `wpa_cli disconnect` and `reconnect`, so
wpa_supplicant joins whichever known network is in range. cfg80211 always
lists the associated access point (it holds that entry), so "heard" means
its "last seen" from this scan is under 3 s; a scan that fails (busy)
leaves WiFi alone.

## D073 A sound when the charger is plugged in or unplugged (2026-10-12)

Plugging in buzzes once and plays a rising doodle-doo; unplugging plays a
falling one and does not buzz. The charger driver reports each change of
VBUS (`power_supply_changed`), so a udev rule on `pm8916-lbc-chgr` runs
`s22-charge-sound` with the new "online" value; the charger also reports
charging, full and the like, so only a change from the last value (kept in
`/run/s22-charge-online`, noted silently at boot) makes a sound. It follows
the notification rules: the ring and notification volume, Sounds off means
no sound, Vibrate off no buzz, and nothing in a call. Plugging in while the
phone is suspended wakes it (D071), so the sound plays then too.

AOSP's own charging sound is a low thump with no unplugged sound to match,
so the tones are made at build time (`charge-tones.py`): three soft mallet
notes, C5 E5 G5, short short long (C6 was too shrill on the loudspeaker).
No audio file is kept in git.

## D074 Match Filed: the BENCsite bubble shooter as a native app (2026-10-12)

BENCO: MATCH FILED (BENCsite's benco-match-filed.html) is `s22-match`, in
the Games folder. It is a C port on s22ui, not the page in WebKit: the
browser engine is slow on the Adreno 308 and holds a lot of memory for one
canvas. The port keeps the page's rules and numbers exactly (grid presets,
the wall every 8 shots, the bomb in danger, scoring, the name checks), its
260 x 310 playfield scaled to the screen's width, and its look: pieces
drawn from the page's shapes by signed distance (smooth edges at any
scale), each once per size and copied after (the page's sprite cache), the
overlays and scanlines. Its Web Audio beeps and noise bursts are mixed in
a thread (square waves, exponential fall to 0.0001, a biquad low-pass on
the noise) into the media volume; the device closes after two quiet
seconds.

For the phone: the keypad aims (a step per press, then turning while held,
faster after 0.65 s; 1 and 3 a hair), OK fires, * the laser sight, Back
pauses; a finger aims and fires as it lifts, as on the page. The side
panel is a bar above the playfield; the roster and instructions are on
the start screen. The best score, the leaderboard and the sight are in
~/.local/share/s22/match-filed (the page used a cookie). It runs its clock
only while something moves, keeps the screen on only during a run, and
pauses when the window is hidden or loses the keyboard.

## D075 Badges: unread texts and missed calls on the home screen (2026-10-12)

A dismissed notification only takes the alert away, so the home screen's
tiles keep count of what waits: an app's `.app` file names a count with
`Badge=` (messages, calls), drawn as a red number at the tile's corner; a
folder shows the sum of its apps'. Counting lives in s22ui (`s22badges.c`)
so every program reads it the same way:

- messages: texts in s22-smsd's inbox newer than their conversation's read
  mark, the one s22-messages already kept (opening a conversation clears
  it). Messages now uses the library's number matching and time parsing,
  so the two cannot disagree.
- calls: missed calls in s22-calld's log newer than `calls-seen` in
  ~/.local/share/s22, which Phone writes when it shows its recent calls.
  It starts at the first count, so older missed calls (seen before badges)
  do not light it.

The counts are redone only when their files change (mtime and size). The
launcher watches the three directories with inotify, so a badge appears as
a text arrives, and redraws when it comes back to the front.

## D076 A private Wi-Fi address for public networks (2026-10-12)

A network the phone joins sees its address; public ones can log it and
recognise it everywhere. Each saved network now has a "Private address"
switch (Settings, the network's page): on, wpa_supplicant joins it with
`mac_addr=3` and a `mac_value` of its own (random, locally administered,
unicast), the same every time it joins that network, so a captive portal
knows it again but two networks cannot match it. Networks without a
password, the public ones, get it when they are added; the user's own
(with a password) keep the phone's address unless switched. Switching it
joins again (disconnect, reconnect) with the new address and a new lease.
Tested on the home network in memory: the address changed, the router
gave a new lease (11 s, against 3-4 s for a known one), traffic passed,
and switching back restored the phone's address. wpa_supplicant reads
back mac_value only as "*" or FAIL, so Settings goes by mac_addr alone,
and a switch turned on again draws a new address.

Scans are not covered: wcn36xx sends every scan to the firmware as an
active scan with the interface's address, and the driver offers no
random scan address, so wpa_supplicant's preassoc_mac_addr and
passive_scan cannot hide it. The lid-open check (D072) no longer names the
network in its probe, so opened somewhere else the phone does not call out
the home network's name.

## D077 The browser and a GPU reset (2026-10-12)

The browser could take the whole GUI down. Its UI process (GTK's GL
renderer) sent the Adreno 308 work that hung it; the msm hangcheck reset
the GPU after 500 ms, naming s22-browser-bin each time. The reset itself
was harmless, but every GL context alive across it was lost: s22-wm spun
on GL_CONTEXT_LOST and never drew again, and the browser kept a dead
window, so C (stop) did nothing and # (typing) crashed it.

- s22-wm 0.79 makes a new renderer and allocator when its renderer reports
  the loss (`events.lost`), moves the clients' buffers to it
  (`wlr_compositor_set_renderer`) and reinitialises the output. The rebuild
  waits for an idle callback: wlroots emits `lost` with its own markers in
  the listener list, and destroying the old renderer inside the handler
  trips wlr_renderer_destroy's empty-list assertion (0.78 did, and the
  service restarted it).
- s22-browser 0.6 draws its window with GTK's cairo renderer
  (`GSK_RENDERER=cairo`, overridable). WebKit's web process still draws
  the pages on the GPU. With it a YouTube page, stop and typing ran without
  a hang. Whether GL hangs only since D061 (the wm waits for a GPU
  client's fences before taking a commit) is not settled; trying
  `GSK_RENDERER=gl` again on top of 0.79 would tell.
- s22-browser 0.7: C hides the loading bar at once and until the next load
  starts; WebKit can report loading for a while after it is told to stop.

## D078 GPS: the modem's memory service (2026-10-12)

The modem's location engine (QMI LOC, service 16 on the modem) ran, but
never saw a satellite: every report was a cell-ID guess (~387 km), with no
satellite or engine-state events, also outdoors with time and XTRA orbits
injected. The host never ran QMI memshare ("Dynamic Heap Memory Sharing",
service 52), which the modem asks at boot: msm8917.dtsi has the service
and a 2 MiB gps region, both disabled, and our board never enabled them.
Stock has the same service (clients GPS, FTM, DIAG).

BENCkernel-CAT 7.1.3-44 enables `&memshare` and `&gps_mem` and builds the
service in (`CONFIG_QCOM_MEMSHARE_QMI_SERVICE=y`), so it answers the
modem's first request at boot. Result, outdoors and cold with no
assistance: engine state at once, a first fix in 28 s, 13 GPS and 7
GLONASS satellites in view, ~3 m uncertainty within a minute.

The gps region itself was never asked for. The only request since boot
was the modem's at 21 s for client 1 (stock's DIAG client, size 0 there
too), which the driver turns away; a whole GPS session sent no allocation
request (driver debug messages on). So what the engine needed was most
likely an answer at all, not the memory. The region stays (2 MiB); a boot
without it would confirm.

Receive only: nothing goes over the cellular network, and nothing on the
phone starts a session by itself.

## D079 s22-gnssd: GPS while something asks (2026-10-12)

With the engine working (D078), s22-gnssd keeps one QMI LOC client (a
session lives only as long as the client that started it) and runs a
session only while a client holds its socket, as s22-celld's data holds
and s22-sensord's sensors do: the receiver is off otherwise, so it costs
nothing idle, and nothing turns it on by itself. `s22-gnss` is the shell's
client (a line per report, `-1` for one fix within 50 m). The state is
key=value lines like s22-celld's, but 0640 root:wheel, since it is where
the phone is; a separate world-readable `active` file (session, fix kind,
no position) changes only when those do, and the status bar draws the
location arrow from it (grey while searching, white with a satellite fix)
without redrawing every second. A warm start had a fix in a few seconds.
Power cost of a session, and fixes indoors and on the move, are not
measured yet.

## D080 Maps: Mepo with the keypad and GPS (2026-10-12)

A map came next with GPS (D078, D079). Of the maintained Linux map apps,
Mepo (SDL2, OpenStreetMap raster tiles, cached on the SD card, keys for
everything) fits a 2 GB keypad phone; Pure Maps (Qt, plus OSM Scout Server
for offline) and CoMaps (desktop build, not for phones yet) are heavier.
Mepo 1.3.4 needs Zig 0.14 and the distribution has 0.16, so the package
builds with the official 0.14.1 release (checksummed source); ~5 min on the
phone.

benctix.patch: bind_key takes single characters only and acts on key
release, so the D-pad could not pan; key presses (and their repeats) of the
arrows now run what h j k l are bound to, and Return what x is. The tile
requests name us in their user agent, as OpenStreetMap's tile policy asks.

s22-maps starts it as `game-maps` (the keypad as a game's keys), sends it
its key bindings on its standard input (`mepo -i`), holds GPS from
s22-gnssd while the map is open, and moves the position pin at every
satellite fix; the first one centres the map. Keys: D-pad and 2 4 6 8 pan,
1 and 3 zoom, 5 or OK back to the position, 0 network tiles on or off.
Not yet: search and routing (Mepo's menus use zenity, not installed), and
offline maps beyond what was viewed (planned: vector data for the US,
drawn natively).

## D081 Maps search, and s22-pick (2026-10-13)

Mepo's own search scripts ask through zenity (GTK dialogs, not installed;
no T9 in them) and parse with jq, and Nominatim's usage policy wants a
user agent that names the program, which curl's default does not. So
Maps has its own: # runs s22-maps-search (benctix.patch maps the keypad's
# to it, Tab in game mode), which asks for the words in s22-pick, and where
(◂ ▸, remembered): within 5, 25 or 100 miles of the GPS fix (or the map's
centre), in the US, or anywhere. It asks two OpenStreetMap searches at
once: Photon (photon.komoot.io), which forgives spelling ("mcdonalds",
"walgrens") and ranks by nearness, and Nominatim, which is better at street
addresses but matches whole words only ("mcdonalds" missed every
McDonald's; words ending in s are also tried with "'s", a second request a
second later). A distance is a bounded box around that point, then the
circle; the US is a country code (Nominatim) or the result's country
(Photon); otherwise nearness only weighs in (Photon's bias loosened, so the
Grand Canyon comes before a sign shop). Up to 40 results from each
(Nominatim's dedupe off: every branch), merged, one place mapped twice (a
point and its building, same name within ~50 m) shown once. Overpass, which
could match names loosely, timed out on both public servers (2026-10-13).
s22-pick lists them with their distance from the GPS fix (or the map's
centre), nearest first within a distance and in the searches' order
otherwise, and gives Mepo the pins (group 0, replacing the last search's) with the
chosen one selected and centred. Nominatim gets one or two requests per
search, a second apart, as its policy asks. Searching sends the words and
where (the GPS fix or the map's view) to both services; offline search
waits for the offline maps.

s22-pick (s22-gui 0.83) is a general question window for scripts: a line
of text (T9, the toolkit's entry), with a choice under it (◂ ▸) if asked,
or one of a list read from standard
input, shown as it arrives ("Searching…" until the input ends); the answer
on standard output, exit status 1 on Back. Routing destinations can use it
later.

benctix.patch also makes pin_add move an existing pin with the same
handle: Mepo ignored the add, so the position pin stayed at the first fix.

## D082 Battery with the lid closed: what woke it, Tailscale paused (2026-10-13)

The first overnight test (lid closed, unplugged, 00:22 to 11:22): 100% to
64%, about 3.3%/h, some 30 hours of standby. Two findings:

- **The gauge froze.** The PM8916 VM-BMS driver's voltage changes only when
  the PMIC's sample FIFO fills, which takes longer than a lid-closed wake
  (about 7 s), and on every resume it hands out its last hardware OCV again
  as if new; the PMIC measures one only after a long sleep. So the driver,
  and s22-battery after it, said 100% until 08:35, then 69%; the
  low-battery check read the same frozen voltage. s22-battery 0.5 reads the
  PMIC ADC's VSYS channel while discharging (converted when read; the
  battery's voltage without a charger), and counts an OCV only when it has
  changed (or was taken at this boot). The log gains vsys_mv.
- **WiFi woke it.** BENCkernel-CAT 7.1.3-45 enables CONFIG_PM_DEBUG for
  /sys/power/pm_wakeup_irq, which s22-sleepd 0.12 logs: of 475 wakes, 459
  were wcn36xx's RX interrupt, 16 the modem's SMD edge. The broadcast and
  multicast filter (D071) was working; what came were unicast packets to
  the phone. A capture of the first frames after each resume (lid closed,
  25 minutes each way): with Tailscale up, its control server (21 wakes)
  and two DERP relays (17, 11), SMB from a computer on the LAN (9); 63
  wakes, 66% asleep. With `tailscale down`: 13 wakes in 68 minutes, 99%
  asleep, one sleep of 48 minutes.

So s22-sleepd 0.13 pauses Tailscale (`tailscale down`) once the lid has
been closed for 2 minutes, and brings it up when the lid opens. The
tailscale_lid setting (Settings, VPN: "Tailscale with the lid closed",
s22-gui 0.84) keeps it connected instead. Only a running Tailscale is
paused, and only one paused by s22-sleepd is brought up; the marker is in
/var/lib/s22-sleep, so a reboot while paused still brings it back. While
paused the phone is not reachable on the tailnet (ssh, Taildrop); USB, the
LAN, calls and texts are unchanged. This amends D036's "always on".

The second night (2026-10-13 23:48 to 10-14 11:57, Samba off): 100% to
88% in 12.2 hours, about 1%/h with the first hour's sag after charging,
99.1% asleep. 56 wakes: 46 the modem, in threes every ~50 minutes (likely
the network's periodic tracking area update), 10 WiFi. What is left is
the current of the sleep itself. The driver's HEALTH=Over voltage is its
own frozen 4.348 V against the device tree's 4.325 V maximum: the charger
is set to 4.325 V (stock) and VSYS read 4.323-4.326 V when full, so the
BMS ADC reads about 20 mV high.

## D083 Music: Artists and Albums by the tags, letters with 1 and 3 (2026-10-14)

Music (s22-gui 0.85) gains Artists and Albums tabs, between Folders and
Songs. They go by the songs' tags, which the app never read before (mpv's
metadata covers only the song playing).

- **Reading.** s22-music-tags reads each song's artist, album artist,
  album, track, disc, year and title with libavformat (ffmpeg is already a
  dependency; headers only, no stream probing) and keeps them in
  ~/.cache/s22-music/tags, a line a song with its time and size. Music
  runs it as it opens and when Artists or Albums opens, in the background
  (niced, a pipe for its progress), and reads the cache again only if it
  read anything. The first look at the 687 songs here took 19 s, CPU-bound;
  later ones 0.25 s. Until the first is done the tabs say how far it is.
- **Albums** are the album artist's, or without one the folder's (so a
  compilation with no album artist stays together, its artist "Various
  artists" when the songs disagree); an album without a name takes its
  folder's. Inside, songs go by disc, track, then file name.
- **Artists** are every name an album artist or a song's artist gives,
  split at ";" (MediaMonkey's separator; not at commas, which are in
  names). An artist's page: All songs, their albums by year, then the
  albums they are on (compilations), which open whole.
- **1 and 3** jump to the previous or next first letter in the lists that
  are by name (Folders, Artists, Albums, Songs, Playlists), the letter
  flashed; 4 and 6 already paged, held for page after page.
- Six tabs at 480 pixels: each as wide as its name plus an equal share of
  the rest, at 16 px (equal widths cut "Playlists").

## D084 Music: a visualizer under the bar in now playing (2026-10-14)

Now playing (s22-gui 0.86, 0.88) shows what plays under the bar: Bars (28 log
bands, 40 Hz to 14 kHz, peaks held 0.4 s), Spectrum (120 bands as a
filled curve) or Scope (20 ms of the wave from a rising zero, so it stands
still). 0 changes it, the music_vis setting keeps it (default bars).
Play or pause sits small between the song's times (the big button is
gone), the "N of M" line at the bottom; the visualizer goes between them
and the bar, which sits lower with it off.

- **The sound** comes from parec on PipeWire's sink monitor
  (@DEFAULT_MONITOR@, mono float at 32 kHz, 20 ms latency) on a pipe the
  app watches, so it sees what reaches the speaker, after mpv's
  normalizer, near when it is heard. mpv itself has no way to hand out
  samples, and a filter writing to a file would sit in the playing path.
- **Only while it shows:** now playing in front, a song playing, the
  visualizer on, and ui_visible() (the compositor answering frames: not
  with the display off, the lid closed or a window over it). Otherwise
  parec is stopped (and dies with the app, PR_SET_PDEATHSIG). Bars fall
  away after it stops, then the frames stop.
- **Frames** are the app's timer at 16 ms (60 a second), paced by the
  compositor's frame callbacks; parec delivers every 10 ms. The FFT (2048
  points, Hann, power summed per band so pink noise is flat, -60 to -8 dB
  shown) takes about 0.5 ms a frame. What stands still (the cover, the
  text with its halo, the bar's panel) is drawn once per song and copied
  each frame: redrawn every frame, the halos alone held s22-music at 95% of
  a core below 30 frames a second; cached, 60 frames take 30%, and s22-wm
  15% to show them. mpv is asked the song's time at most every 400 ms.
- projectM (MilkDrop's presets) in place of the cover is a later step
  (TODO).

## D085 Calls: other sound ducked, then stopped; keypad tones (2026-10-14)

Until now music went on through a call, on the loudspeaker beside the
voice (and on the earpiece while a dialled call rang out, since the
ringback moved the card's media playback there).

- **Ringing:** s22-calld (0.16, 0.17) turns every other stream in the
  GUI's PipeWire down 12 dB from where it was (pactl, as root, on its Pulse
  socket). Pulse volumes are cubic, so that is 63% of the stream's volume;
  0.16's "25%" was -36 dB, too much (the user).
- **A call with audio** (answered, or dialled): those streams are muted
  at once, then the MPRIS players playing are paused (playerctl, as the
  GUI's user on its D-Bus); "pactl subscribe" mutes any stream that starts
  during the call. When the calls end, volumes and mutes are put back and
  only the players paused here play again. Left alone: the internal nodes
  (s22.*), and our own streams by application.name: s22-play (the
  ringtone, s22-say) and s22-calld (ringback, tones). A thread does it, so
  the call handling never waits for it; tried on the phone against Music:
  25%, muted and paused, a new stream muted, all back.
- **Where the card's media playback goes:** MultiMedia1 (under s22.speaker)
  now follows the voice for the whole call (earpiece, or the loudspeaker
  with speakerphone), set by Audio, not only during the ringback; reset to
  the loudspeaker when the call ends and when s22-calld starts.
- **Keypad tones:** the Phone app (s22-gui 0.89, 0.90) plays each key's
  DTMF tone (697-941 Hz with 1209-1477 Hz, 120 ms) through s22-play at the
  media volume (the volume setting on the same cubic scale s22-wm gives the
  sound server; none at 0), unless sounds=off; in a call s22-calld plays the digit it sends
  for DTMF_MS at the ear (aplay into PipeWire, at the ringback's level).

## D086 Going public: a snapshot repo, UTC by default (2026-10-14)

The work goes on in the private repo (renamed BENCtix-CAT-dev on GitHub),
its history as it was. The public bropple/BENCtix-CAT starts from a single
commit, a snapshot of this tree: nothing of the history is published.

Before that, every blob and commit message in the history was searched:
phone numbers, IMEI/IMSI/ICCID-like numbers, MAC and IP addresses,
coordinates, cell IDs, emails and identities, WiFi names and passwords,
keys and tokens, home paths, every file name for firmware or vendor files,
and every artist, album, song, folder and playlist name in the phone's
music library (the user's listening stays private). Nothing phone-derived
was found; the only phone number is a 555 placeholder, the identities are
the GitHub noreply address, and the music names matched only ordinary
words. What the tree had, now gone: the phone's address on the home LAN
in MANUAL.md, the image's timezone (the builder's own)
in config.sh and a search example naming the city, and tracked Python
bytecode. Older history also has early build output with host paths; it
stays private.

config.sh now sets TIMEZONE=UTC and reads an untracked config.local.sh
last, where a builder sets their own (the user keeps theirs there). The tree
is kept clean of these so that a public snapshot is just a copy of it.
A second review (2026-10-14) found the builder's UTC offset in the kernel
manifests' build times, the processors PDF's dates and two format examples
in comments: all are UTC now, build-benckernel-cat.sh writes UTC (date -u)
and the PDF is made with TZ=UTC. The public commit is dated in UTC too.

## D087 s22-wm headless and keys over its socket, for screenshots (2026-10-14)

To show the system as it is, it runs on the build host in a bubblewrap
sandbox: a copy of the phone's root without personal data, dummy data
(invented contacts on 555-01xx numbers, generated music and pictures), as
the user, without network or host devices, through qemu-user (binfmt).
s22-wm (s22-gui 0.91) runs on wlroots' headless backend with the pixman
renderer: nothing touches the host's display or GPU.

- A display without modes (headless) gets S22_WM_SIZE, 480x640 by default.
- The control socket takes "key CODE..." (pressed and released in turn),
  "keydown CODE" and "keyup CODE": evdev codes through a keyboard of
  s22-wm's own, taken like the keypad's, for scripts and such sessions.

What the sandbox fakes, so Settings shows the phone, not the host: the
kernel's release (QEMU_UNAME), /proc/meminfo and /proc/uptime, / and /home
as tmpfs the size of the phone's free eMMC and card; the hardware daemons'
state files (battery, cell, Bluetooth, GPS, a call), and iw's link.
WebKit runs without its own sandbox there (none nests in bubblewrap) and
with software GL. The tooling stays local; a dozen of the screenshots (dummy
data only, PNGs without metadata) are in docs/screenshots for the README.

## D088 Installing without root: the phone's files through lk2nd (2026-10-14)

INSTALL.md takes a stock phone to BENCtix-CAT with an unlocked bootloader
and no root. Root was only ever how this project read partitions; lk2nd's
fastboot reads any partition (`fastboot fetch`, max-fetch-size 0x1ff00000):
persist fetched from it matched the running system's SHA-256 byte for byte.

- tools/fetch-from-lk2nd.sh reads the backup (persist, modemst1, modemst2,
  fsg, fsc) and what the firmware comes from (modem, dtbobak, super), with
  checksums; it checks the device is lk2nd and writes nothing to the phone.
- The stock dtbo survives lk2nd's: the phone has a dtbobak partition, still
  byte-identical to the stock (and OTA V30) dtbo after lk2nd's went into
  dtbo. It is what the outer screen's init sequence is read from.
- tools/extract-firmware.sh lays the firmware out where config.sh points
  (mtools, debugfs, lpunpack, mkdtboimg, pil-squasher, s22flip-linux's
  mkmipidbi.py and sns-reg-groups.py; the GPU microcode from linux-firmware,
  downloaded if the PC has none). Run on this phone's backup it produced a
  tree identical to the firmware in use, sns.reg and the group table too.
- Only a way back to Android needs more: the stock boot.img, from the OTA
  packages shared on XDA (thread 4626971: V29 full and V30 incremental, rebuilt with
  imgpatch.py; their SHA-256s are in INSTALL.md) or saved with root first.
  It goes back from the stock fastboot, since lk2nd's writes behind itself.
