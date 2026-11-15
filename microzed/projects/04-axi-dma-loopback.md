# 04: AXI DMA loopback

## Goal

Send a buffer from Linux into the PL and back again through an AXI DMA and a FIFO, using the kernel's DMA engine framework, and check the data arrives intact.

## Why

DMA moves data between the PL and DDR memory without the CPU copying it. Every high-speed data path (ADC, camera, network offload) is built on it. This is the DMA goal on the roadmap.

## Builds on

- 02 and 03 (custom IP, device tree, interrupts).
- Kernel driver basics from `microzed/modules/`.

## Steps (outline)

1. Block design: AXI DMA (MM2S + S2MM, no scatter-gather to start) connected through an AXI4-Stream Data FIFO in a loop; DMA control on M_AXI_GP0; DMA memory access through an HP port (S_AXI_HP0); DMA interrupts to IRQ_F2P.
2. Kernel: enable the Xilinx DMA engine driver (`CONFIG_XILINX_DMA`) if it is not already on (`make linux-menuconfig`).
3. Device tree: the AXI DMA node (`compatible = "xlnx,axi-dma-1.00.a"`, channels, interrupts, clocks) plus a node for our client driver that refers to the two channels (`dmas`, `dma-names = "tx", "rx"`).
4. Client driver `microzed/modules/dmaloop/`: `dma_request_chan()`, buffers from `dma_alloc_coherent()`, `dmaengine_prep_slave_single()`, completion callbacks, then compare buffers. Trigger from a sysfs file or `write()`.
5. Measure throughput for several buffer sizes.

## Done when

- A loopback of, for example, 64 KiB returns identical data, repeatedly.
- Throughput numbers are recorded in the md.

## Pitfalls

- HP ports are not cache-coherent: use the DMA API correctly (coherent buffers, or `dma_map_single()` + sync), never plain `kmalloc` buffers without mapping.
- Start the receive (S2MM) transfer before the transmit, or the FIFO fills and stalls.
- The AXI DMA "buffer length register width" in Vivado limits the largest single transfer.
- If the HP port or its clock is not set up by U-Boot's `ps7_init`, transfers hang: check the PS configuration assumptions from project 02.
