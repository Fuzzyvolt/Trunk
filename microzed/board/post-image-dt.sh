#!/bin/sh
# Buildroot post-image script: add our own nodes to the board's device tree
# blob (.dtb) before genimage packs it into sdcard.img.
#
# Set in Buildroot menuconfig (System configuration -> Custom scripts to run
# after creating filesystem images), BEFORE the existing genimage script:
#   /home/fabio/trunk/microzed/board/post-image-dt.sh support/scripts/genimage.sh
#
# Buildroot calls it with the images folder as $1 (extra arguments are ignored).
# Needs fdtget/fdtput/dtc on the PC:  sudo apt install device-tree-compiler
#
# The node it adds is the same as writing this in the .dts source:
#
#   / {
#       myled {
#           compatible = "microzed,myled";
#           label = "d3";
#           led-gpios = <&gpio0 47 0>;    /* MIO47, 0 = active high */
#       };
#   };
set -e

IMAGES=${1:?usage: $0 <images-dir>}
GPIO_NODE=/axi/gpio@e000a000

for t in fdtget fdtput dtc; do
  command -v $t >/dev/null || { echo "post-image-dt: $t missing (sudo apt install device-tree-compiler)"; exit 1; }
done

found=0
for DTB in "$IMAGES"/*.dtb; do
  [ -f "$DTB" ] || continue
  found=1

  if fdtget "$DTB" /myled compatible >/dev/null 2>&1; then
    echo "post-image-dt: $(basename "$DTB") already has /myled"
    continue
  fi
  fdtget "$DTB" $GPIO_NODE compatible >/dev/null 2>&1 \
    || { echo "post-image-dt: $GPIO_NODE not found in $DTB"; exit 1; }

  # A reference to another node (&gpio0) is stored as that node's "phandle"
  # number. If the GPIO controller has none yet, give it an unused one.
  PH=$(fdtget -t u "$DTB" $GPIO_NODE phandle 2>/dev/null || true)
  if [ -z "$PH" ]; then
    MAX=$(dtc -q -I dtb -O dts "$DTB" \
      | sed -n 's/.*phandle = <\(0x[0-9a-fA-F]*\)>.*/\1/p' \
      | while read -r h; do printf '%d\n' "$h"; done | sort -n | tail -n1)
    PH=$(( ${MAX:-0} + 1 ))
    fdtput -t u "$DTB" $GPIO_NODE phandle $PH
  fi

  fdtput -c "$DTB" /myled
  fdtput -t s "$DTB" /myled compatible "microzed,myled"
  fdtput -t s "$DTB" /myled label "d3"
  fdtput -t u "$DTB" /myled led-gpios $PH 47 0
  echo "post-image-dt: added /myled to $(basename "$DTB") (gpio phandle $PH)"
done

[ $found = 1 ] || { echo "post-image-dt: no .dtb in $IMAGES"; exit 1; }
