#!/bin/sh
# Flash the Buildroot SD card image in one step:
# safety checks -> unmount -> dd -> sync -> eject.
#
# Usage:  ./flash-sd.sh [device]        (default device: /dev/sdb)
#         IMG=/path/to/sdcard.img ./flash-sd.sh /dev/sdc
set -e

IMG=${IMG:-$HOME/br-microzed/images/sdcard.img}
DEV=${1:-/dev/sdb}

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "image not found: $IMG"
[ -b "$DEV" ] || die "$DEV is not a block device (is the card inserted? check with lsblk)"

# Never touch the disk the PC is running from.
if lsblk -lno MOUNTPOINT "$DEV" | grep -qxE '/|/boot|/boot/efi|/home|\[SWAP\]'; then
  die "$DEV holds a system partition of this PC. Refusing."
fi

# Must look like a removable card: USB reader, SD/MMC reader, or removable flag.
RM=$(lsblk -dno RM "$DEV" | tr -d ' ')
TRAN=$(lsblk -dno TRAN "$DEV" | tr -d ' ')
case "$DEV" in /dev/mmcblk*) TRAN=mmc ;; esac
[ "$RM" = "1" ] || [ "$TRAN" = "usb" ] || [ "$TRAN" = "mmc" ] \
  || die "$DEV does not look like a removable card (RM=$RM, TRAN=$TRAN). Refusing."

# Cards over 64 GB are unusual here; stop in case it's an external drive.
SIZE=$(lsblk -bdno SIZE "$DEV" | tr -d ' ')
[ "$SIZE" -le 68719476736 ] || die "$DEV is larger than 64 GB. Refusing."

echo "Image : $IMG ($(date -r "$IMG" '+%Y-%m-%d %H:%M'))"
echo "Target:"
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINT "$DEV"
printf "\nEverything on %s will be erased. Type 'yes' to continue: " "$DEV"
read -r ANSWER
[ "$ANSWER" = "yes" ] || die "cancelled"

echo "Unmounting partitions..."
for p in $(lsblk -lnpo NAME "$DEV" | tail -n +2); do
  sudo umount "$p" 2>/dev/null || true
done

echo "Writing image..."
sudo dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
sync

sudo eject "$DEV" 2>/dev/null || true
echo "Done. You can remove the card."
