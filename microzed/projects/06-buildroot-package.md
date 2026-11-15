# 06: Buildroot package for our modules (optional)

## Goal

Build our kernel modules and programs as Buildroot packages in a `BR2_EXTERNAL` tree inside the repo, so they are rebuilt with the kernel automatically and load with `modprobe`.

## Why

Today the modules are built by our own Makefile and copied through the overlay, so `depmod` never sees them (`insmod` with a full path in `S30myled`), and a kernel change needs a manual module rebuild. A package fixes both, and it is how real products ship their drivers.

## Builds on

- `microzed/modules/myled-irq/`, `microzed/apps/`, `microzed/Makefile`.
- Buildroot setup in `buildroot-microzed-project.md`.

## Steps (outline)

1. Create `microzed/br2-external/` with `external.desc`, `external.mk`, `Config.in`.
2. Package `myled-irq` with Buildroot's kernel-module infrastructure (`$(eval $(kernel-module))` then `$(eval $(generic-package))`), source from the repo (`*_SITE_METHOD = local`).
3. Package the user-space programs (`hello`, `blink`) as generic packages.
4. Point Buildroot at it: `make BR2_EXTERNAL=/home/fabio/trunk/microzed/br2-external menuconfig` (remembered after the first time), enable the packages.
5. Replace `insmod` with `modprobe myled_irq` in `S30myled`, or enable mdev so the module loads automatically from the device tree `compatible` (modalias).
6. Update `microzed/Makefile` and the README so `make flash` uses the packages.

## Done when

- A clean `make` in `~/br-microzed` builds and installs the module under `/lib/modules/<kernel version>/`.
- `modprobe myled_irq` works on the board (and optionally the module loads by itself at boot).

## Pitfalls

- `BR2_EXTERNAL` is stored in the build folder's config; moving the repo breaks the path.
- After changing package sources: `make <pkg>-rebuild`.
