# Installing BENCtix-CAT

From a stock Cat S22 Flip to BENCtix-CAT. You need an unlocked bootloader,
not root: everything the system needs from your phone is read back through
lk2nd once it is installed.

> [!CAUTION]
> This replaces Android, and it can brick a phone if you go off the path.
> - **Never flash `sbl1`, `tz`, `rpm`, `aboot`, `devcfg`, or any partition
>   these steps do not name.** The chain of trust is enforced in hardware;
>   a broken bootloader partition has no known recovery.
> - **Keep the backup from step 4 safe and private.** It holds your IMEI and
>   radio calibration, and nobody else's copy works on your phone.

## What you need

- A Cat S22 Flip and a USB-C cable; optionally a microSD card for `/home`.
- An x86_64 Linux PC (Arch-family for the build) with android-tools
  (`fastboot`, `lpunpack`, `mkdtboimg`), `mtools`, `e2fsprogs`, `git`,
  `python3`, `qemu-user-static` registered for aarch64, and the cross
  compilers `aarch64-linux-gnu-gcc` and `arm-none-eabi-gcc`.
- [pil-squasher](https://github.com/linux-msm/pil-squasher), built.
- This repository and [s22flip-linux](https://github.com/bropple/s22flip-linux)
  checked out side by side.

## 1. Unlock the bootloader

Unlocking **erases Android's data** (not the IMEI or calibration).

1. In Android, enable Developer options, then **OEM unlocking**.
2. Power off, then hold **Volume Down + Power** for the stock fastboot.
3. Run `fastboot flashing unlock` and confirm on the phone.

## 2. Optional: a way back to Android

Installing lk2nd replaces the `boot` and `dtbo` partitions. The stock `dtbo`
needs nothing from you: the phone keeps a copy in `dtbobak`, which nothing
here touches. To go back to Android later, you also need the stock
`boot.img`, from either:

- **Cat's OTA packages**, as shared on XDA Developers in
  [this thread](https://xdaforums.com/t/tut-root-how-to-root-cat-s22-flip-on-version-30.4626971/):
  the V29 full OTA and the V30 incremental. Rebuild V30's images with s22flip-linux's
  `tools/imgpatch.py` (see its HARDWARE.md). These are the files this
  project used:

  | File | SHA-256 |
  |---|---|
  | V29 full OTA | `c22aff2c972e04c757c934cd6689182b398f91e96da4f3a2ec4ef7b83ff0b18d` |
  | V30 incremental | `2d40753316e9def49d4aca70ec289480be396a28032703097efdf64e127743fb` |

- **Root, before step 3:** a Magisk-patched stock boot and s22flip-linux's
  `tools/backup_partitions.sh` save every partition, `boot` included.

## 3. Install lk2nd

The stock bootloader cannot start a mainline kernel. It starts
[lk2nd](https://github.com/bropple/lk2nd/tree/s22flip) from the `boot`
partition, which boots Linux and offers its own fastboot. The stock
bootloader also needs a minimal `dtbo` before it will start lk2nd.

```
git clone -b s22flip https://github.com/bropple/lk2nd
make -C lk2nd TOOLCHAIN_PREFIX=arm-none-eabi- lk2nd-msm8952
git clone -b s22flip https://github.com/bropple/dtbo-lk2nd
make -C dtbo-lk2nd build/dtbo-cat-s22flip.img
```

From the stock fastboot (power off, then Volume Down + Power):

```
fastboot flash dtbo dtbo-lk2nd/build/dtbo-cat-s22flip.img
fastboot flash boot lk2nd/build-lk2nd-msm8952/lk2nd.img
fastboot reboot
```

lk2nd reports `Cat S22 Flip (S22FLIP)`. With nothing installed yet it stays
in its own fastboot, which `fastboot getvar product` shows as
`lk2nd-msm8952`. Use only lk2nd's fastboot from here on.

## 4. Back up your partitions and fetch the firmware

```
tools/fetch-from-lk2nd.sh ~/s22-parts
```

This reads (and never writes) `persist`, the modem's EFS (`modemst1`,
`modemst2`, `fsg`, `fsc`), `modem`, `dtbobak` and `super` into
`~/s22-parts`, with checksums. It takes a few minutes, mostly `super`.
**This folder is your backup:** keep a copy somewhere safe and never publish
it.

## 5. Lay out the firmware

```
PIL_SQUASHER=/path/to/pil-squasher tools/extract-firmware.sh ~/s22-parts
```

This writes the signed firmware, WiFi calibration, amplifier profile,
sensor registry and outer-screen init sequence where the build looks for
them (`../firmware`, set in `config.sh`). It also gets the GPU microcode
from linux-firmware. These files are your phone's own and stay on your PC;
the build copies them into the image.

## 6. Build

Put your own settings in `config.local.sh` (untracked), at least your
timezone:

```
TIMEZONE=Europe/Berlin
```

Your ssh public key (`~/.ssh/id_ed25519.pub`, `SSH_AUTHORIZED_KEYS_FILE`)
goes into the image for root and the owner account (`micro` unless you set
`OWNER_USER`).

Then build the kernel and the root filesystem as the [README](README.md#building)
describes: `tools/build-benckernel-cat.sh`, then `stages/10` to `50`.

## 7. Flash

With the phone in lk2nd's fastboot:

```
tools/flash-benctix-cat.sh
```

It shows what it will write, asks first, and writes exactly two partitions:
`cache` (`/boot`) and `userdata` (`/`). It never touches `boot`, the
bootloaders or the modem's partitions.

## 8. First boot

- The first boot copies the modem's EFS from the eMMC into
  `/var/lib/rmtfs`, reading it only. From then on the modem uses those
  files.
- Over USB the phone is `172.16.42.1`: `ssh root@172.16.42.1` or
  `ssh micro@172.16.42.1` with your key. Set your password with `passwd`;
  the lock screen PIN is set in Settings, Security.
- `/home` is the microSD card if it is formatted f2fs with the label
  `s22home` (`mkfs.f2fs -l s22home`); without one, it lives on the eMMC.
- To reach lk2nd's fastboot again from the running system, use
  `reboot-lk2nd`.
- How to use the phone: [MANUAL.md](MANUAL.md).

## Going back to Android

From the **stock** fastboot (power off, then Volume Down + Power), not
lk2nd's: lk2nd's own `fastboot flash boot` writes behind lk2nd and keeps it.
Flash the stock images back and wipe what BENCtix-CAT used:

```
fastboot flash dtbo ~/s22-parts/dtbobak.img
fastboot flash boot boot.img            # the stock boot.img from step 2
fastboot erase cache
fastboot erase userdata
```

Then `fastboot reboot`: Android starts as after a factory reset.
