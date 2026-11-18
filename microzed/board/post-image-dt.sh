#!/bin/sh
# Buildroot post-image script: add our own nodes to the board's device tree
# blob (.dtb) before genimage packs it into sdcard.img.
#
# Set in Buildroot menuconfig (System configuration -> Custom scripts to run
# after creating filesystem images), BEFORE the existing genimage script:
#   /home/fabio/microzed-linux/microzed/board/post-image-dt.sh board/zynq/post-image.sh
#
# Buildroot calls it with the images folder as $1 (extra arguments are ignored).
# Needs fdtget/fdtput/dtc on the PC:  sudo apt install device-tree-compiler
#
# The nodes it adds are the same as writing this in the .dts source:
#
#   / {
#       myled {
#           compatible = "microzed,myled";
#           label = "d3";
#           led-gpios = <&gpio0 47 0>;    /* MIO47, 0 = active high */
#           button-gpios = <&gpio0 51 0>; /* MIO51, PB1 */
#       };
#       axiregs@43c00000 {                 /* project 02: registers in the PL */
#           compatible = "microzed,axiregs";
#           reg = <0x43c00000 0x1000>;     /* address and size set in Vivado */
#           clocks = <&clkc 15>;           /* FCLK_CLK0 */
#           fpga-mgr = <&devcfg>;          /* lets the driver check the PL is loaded */
#       };
#       pltimer@43c10000 {                 /* project 03: timer with interrupt */
#           compatible = "microzed,pltimer";
#           reg = <0x43c10000 0x1000>;
#           interrupt-parent = <&intc>;    /* the GIC */
#           interrupts = <0 29 4>;         /* SPI 29 = GIC ID 61 = IRQ_F2P[0], level high */
#           clocks = <&clkc 15>;
#           fpga-mgr = <&devcfg>;
#       };
#       axi_dma: dma@40400000 {            /* project 04: AXI DMA in the PL */
#           compatible = "xlnx,axi-dma-1.00.a";
#           reg = <0x40400000 0x10000>;
#           #dma-cells = <1>;              /* clients say <&axi_dma N>, N = channel */
#           interrupt-parent = <&intc>;
#           clocks = <&clkc 15>, <&clkc 15>, <&clkc 15>;
#           clock-names = "s_axi_lite_aclk", "m_axi_mm2s_aclk", "m_axi_s2mm_aclk";
#           xlnx,addrwidth = <32>;
#           xlnx,sg-length-width = <23>;   /* = "buffer length register width" in Vivado */
#           dma-channel@40400000 {         /* channel 0: MM2S, memory -> stream */
#               compatible = "xlnx,axi-dma-mm2s-channel";
#               interrupts = <0 30 4>;     /* IRQ_F2P[1] = GIC ID 62 */
#               xlnx,datawidth = <32>;
#           };
#           dma-channel@40400030 {         /* channel 1: S2MM, stream -> memory */
#               compatible = "xlnx,axi-dma-s2mm-channel";
#               interrupts = <0 31 4>;     /* IRQ_F2P[2] = GIC ID 63 */
#               xlnx,datawidth = <32>;
#           };
#       };
#       dmaloop {                          /* project 04: our DMA client driver */
#           compatible = "microzed,dmaloop";
#           dmas = <&axi_dma 0>, <&axi_dma 1>;
#           dma-names = "tx", "rx";
#       };
#   };
#
# DANGER: the Xilinx DMA driver reads the DMA's registers as soon as it binds.
# If it is built into the kernel (=y, the xilinx_zynq_defconfig default) it
# binds at boot, before the PL is programmed, and the CPU hangs. It must be a
# module (=m, configs/linux.fragment), loaded by S40pl after the bitstream.
# This script checks the kernel .config and writes the DMA nodes with
# status = "disabled" if the driver is not a module.
#
# GIC interrupt numbers: IDs 0-15 are SGIs (CPU to CPU), 16-31 PPIs (per-CPU,
# like the private timers), 32 and up SPIs (shared: peripherals). The device
# tree counts SPIs from 0, so "0 29" (type 0 = SPI) means GIC ID 32 + 29 = 61.
# On the Zynq, IRQ_F2P[0..7] are GIC IDs 61-68 and IRQ_F2P[8..15] are 84-91.
# Last cell: 1 = rising edge, 4 = level high.
set -e

IMAGES=${1:?usage: $0 <images-dir>}
GPIO_NODE=/axi/gpio@e000a000

CLKC_NODE=/axi/slcr@f8000000/clkc@100
DEVCFG_NODE=/axi/devcfg@f8007000
INTC_NODE=/axi/interrupt-controller@f8f01000
AXIREGS=/axiregs@43c00000
PLTIMER=/pltimer@43c10000
DMA=/dma@40400000
DMALOOP=/dmaloop

# Buildroot exports BUILD_DIR to post-image scripts. Find the kernel .config.
KCONFIG=$(ls -d "${BUILD_DIR:-$HOME/br-microzed/build}"/linux-* 2>/dev/null \
  | grep -v -e headers -e firmware | head -n1)/.config
if grep -q '^CONFIG_XILINX_DMA=m' "$KCONFIG" 2>/dev/null; then
  DMA_STATUS=okay
else
  DMA_STATUS=disabled
  echo "post-image-dt: WARNING: CONFIG_XILINX_DMA is not =m in $KCONFIG"
  echo "post-image-dt: WARNING: DMA nodes written with status = \"disabled\" (see configs/linux.fragment)"
fi

for t in fdtget fdtput dtc; do
  command -v $t >/dev/null || { echo "post-image-dt: $t missing (sudo apt install device-tree-compiler)"; exit 1; }
done

# A reference to another node (&gpio0) is stored as that node's "phandle"
# number. Print the node's phandle; if it has none yet, give it an unused one.
phandle() {
  fdtget "$DTB" "$1" compatible >/dev/null 2>&1 \
    || { echo "post-image-dt: $1 not found in $DTB" >&2; exit 1; }
  ph=$(fdtget -t u "$DTB" "$1" phandle 2>/dev/null || true)
  if [ -z "$ph" ]; then
    max=$(dtc -q -I dtb -O dts "$DTB" \
      | sed -n 's/.*phandle = <\(0x[0-9a-fA-F]*\)>.*/\1/p' \
      | while read -r h; do printf '%d\n' "$h"; done | sort -n | tail -n1)
    ph=$(( ${max:-0} + 1 ))
    fdtput -t u "$DTB" "$1" phandle $ph
  fi
  echo $ph
}

found=0
for DTB in "$IMAGES"/*.dtb; do
  [ -f "$DTB" ] || continue
  found=1

  GPIO_PH=$(phandle $GPIO_NODE)
  CLKC_PH=$(phandle $CLKC_NODE)
  DEVCFG_PH=$(phandle $DEVCFG_NODE)
  INTC_PH=$(phandle $INTC_NODE)

  # Create each node if needed, then (re)write all its properties, so running
  # this again on an already-modified .dtb updates it instead of failing.
  fdtget "$DTB" /myled compatible >/dev/null 2>&1 || fdtput -c "$DTB" /myled
  fdtput -t s "$DTB" /myled compatible "microzed,myled"
  fdtput -t s "$DTB" /myled label "d3"
  fdtput -t u "$DTB" /myled led-gpios $GPIO_PH 47 0
  fdtput -t u "$DTB" /myled button-gpios $GPIO_PH 51 0

  fdtget "$DTB" $AXIREGS compatible >/dev/null 2>&1 || fdtput -c "$DTB" $AXIREGS
  fdtput -t s "$DTB" $AXIREGS compatible "microzed,axiregs"
  fdtput -t x "$DTB" $AXIREGS reg 43c00000 1000
  fdtput -t u "$DTB" $AXIREGS clocks $CLKC_PH 15
  fdtput -t u "$DTB" $AXIREGS fpga-mgr $DEVCFG_PH

  fdtget "$DTB" $PLTIMER compatible >/dev/null 2>&1 || fdtput -c "$DTB" $PLTIMER
  fdtput -t s "$DTB" $PLTIMER compatible "microzed,pltimer"
  fdtput -t x "$DTB" $PLTIMER reg 43c10000 1000
  fdtput -t u "$DTB" $PLTIMER interrupt-parent $INTC_PH
  fdtput -t u "$DTB" $PLTIMER interrupts 0 29 4
  fdtput -t u "$DTB" $PLTIMER clocks $CLKC_PH 15
  fdtput -t u "$DTB" $PLTIMER fpga-mgr $DEVCFG_PH

  # Project 04: the AXI DMA (a node with two channel subnodes) and our client.
  fdtget "$DTB" $DMA compatible >/dev/null 2>&1 || fdtput -c "$DTB" $DMA
  fdtput -t s "$DTB" $DMA compatible "xlnx,axi-dma-1.00.a"
  fdtput -t x "$DTB" $DMA reg 40400000 10000
  fdtput -t u "$DTB" $DMA '#dma-cells' 1
  fdtput -t u "$DTB" $DMA interrupt-parent $INTC_PH
  fdtput -t u "$DTB" $DMA clocks $CLKC_PH 15 $CLKC_PH 15 $CLKC_PH 15
  fdtput -t s "$DTB" $DMA clock-names s_axi_lite_aclk m_axi_mm2s_aclk m_axi_s2mm_aclk
  fdtput -t u "$DTB" $DMA xlnx,addrwidth 32
  fdtput -t u "$DTB" $DMA xlnx,sg-length-width 23
  fdtput -t s "$DTB" $DMA status $DMA_STATUS
  # Channel subnode name, compatible, SPI number (IRQ_F2P[1] = 30, [2] = 31;
  # IRQ_F2P[0] = 29 is pltimer)
  for ch in "dma-channel@40400000 xlnx,axi-dma-mm2s-channel 30" \
            "dma-channel@40400030 xlnx,axi-dma-s2mm-channel 31"; do
    set -- $ch
    fdtget "$DTB" $DMA/$1 compatible >/dev/null 2>&1 || fdtput -c "$DTB" $DMA/$1
    fdtput -t s "$DTB" $DMA/$1 compatible $2
    fdtput -t u "$DTB" $DMA/$1 interrupts 0 $3 4
    fdtput -t u "$DTB" $DMA/$1 xlnx,datawidth 32
  done
  DMA_PH=$(phandle $DMA)

  fdtget "$DTB" $DMALOOP compatible >/dev/null 2>&1 || fdtput -c "$DTB" $DMALOOP
  fdtput -t s "$DTB" $DMALOOP compatible "microzed,dmaloop"
  fdtput -t u "$DTB" $DMALOOP dmas $DMA_PH 0 $DMA_PH 1
  fdtput -t s "$DTB" $DMALOOP dma-names tx rx
  fdtput -t s "$DTB" $DMALOOP status $DMA_STATUS

  echo "post-image-dt: /myled, $AXIREGS, $PLTIMER, $DMA ($DMA_STATUS), $DMALOOP set in $(basename "$DTB")"
done

[ $found = 1 ] || { echo "post-image-dt: no .dtb in $IMAGES"; exit 1; }
