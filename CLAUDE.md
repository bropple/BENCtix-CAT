# BENCtix-CAT rules

BENCtix-CAT is Arch Linux ARM with OpenRC for the Cat S22 Flip (QM215), with
its own kernel, BENCkernel-CAT. Hardware bring-up lives in the parent
`cats22/` directory (`HARDWARE.md`); this repo builds the distribution.

## Never

- **Never write to eMMC partitions other than `userdata` (root) and `cache`
  (`/boot`).** Every other partition is read-only from Linux, including
  `boot` (lk2nd), `dtbo`, `super`, `persist` and the modem EFS partitions:
  rmtfs serves the EFS from files in `/var/lib/rmtfs` (D007).
- **Never set eMMC permanent write protection** (`perm`), and never enable
  secure write protection mode. Only power-on protection, which a reset
  clears.
- **Never flash the stock bootloaders** (`sbl1`, `tz`, `rpm`, `aboot`,
  `devcfg` and their backups), from Linux or from fastboot.
- **Never modify `../../BENCphone`.** BENCtix is the model, read-only.
- **Never publish** anything derived from the phone or stock firmware:
  firmware blobs, `sns.reg`, the sensor registry group table, stock vendor
  files, the partition backup, IMEIs, serials or MAC addresses. They are
  installed from local copies only.
- **Never use leaked Qualcomm source** (`adsp_proc`, `vendor_qcom_proprietary`
  and the like on GitHub). Protocols are worked out clean-room from the
  phone's own binaries and live traffic.
- **No upstream contributions** (no patches, PRs, issues or mailing-list
  mail). Work stays in the bropple/* forks.

## Always

- Commit with the noreply identity
  (`bropple <36946060+bropple@users.noreply.github.com>`), and scan for
  private data before pushing.
- Before anything on the phone needs the user (listening, pressing, moving,
  or a step that may need a manual reset), say so and wait for their go.
- Record decisions in `docs/DECISIONS.md` with the reason.
- Prefer overwrite, `rmdir` and `mv -n` over `rm`; list targets before any
  delete; never delete under `../backup`, `../ota`, `../dump`.

## Build host

BENCBENCH (x86_64, Artix, zsh). The kernel is cross-compiled with
`aarch64-linux-gnu-` and packaged with `makepkg` as an aarch64 package.
Scripts are bash; run bash-specific snippets through `bash -c` from zsh.
