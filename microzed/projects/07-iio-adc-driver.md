# 07: The ADC stream as an IIO device

## Goal

Turn `adcstream` (project 05) into a standard Linux **IIO** (Industrial I/O) driver with a buffered channel, so the generic IIO tools read it: `iio_info`, `iio_readdev`, `iio_attr` (libiio), and the sysfs layout every ADC driver in the kernel uses.

## Why

IIO is the kernel's framework for ADCs, DACs and sensors (the XADC already uses it: `/sys/bus/iio/devices/iio:device0`). A real ADC driver would be written this way. Applications then work with any IIO ADC, not only ours, and get standard features for free: channel descriptions (scale, type, sign), triggers, timestamps, and a DMA buffer interface.

## Builds on

- 05: `modules/adcstream/adcstream.c` (DMA ring, sysfs settings), `adcsim` in the PL, the project 05 bitstream (no FPGA change needed).
- 06: the module as a Buildroot package (modprobe). Not required, but makes it easier.

## Steps (outline)

1. Kernel config: `CONFIG_IIO`, `CONFIG_IIO_BUFFER`, `CONFIG_IIO_BUFFER_DMA`, `CONFIG_IIO_BUFFER_DMAENGINE` (check what `xilinx_zynq_defconfig` already has; add to `configs/linux.fragment`). Buildroot: `libiio` with tools.
2. Read how an existing DMA-based IIO ADC driver does it (for example `drivers/iio/adc/adi-axi-adc.c` and the `iio_dmaengine_buffer` helpers) before designing.
3. New driver (`modules/adciio/`, keep `adcstream` for comparison): `iio_dev` with one voltage channel, 16-bit signed, storage 32 bits (the sequence number in the top half, or decide to drop it), `sampling_frequency` as an IIO attribute, the sine/saw mode and frequency as extended attributes.
4. Buffer: start with `devm_iio_dmaengine_buffer_setup()` on our S2MM channel and see how it fits the simple-mode DMA (one buffer at a time, see the project 05 notes); fall back to our own ring feeding a kfifo buffer if it does not.
5. Test with `iio_info`, `iio_readdev -s 100000 iio:device1 > data.bin`, and the sysfs files under `/sys/bus/iio/devices/iio:deviceN/`.
6. Measure: highest lossless rate vs. project 05's `read()` path.

## Done when

- `iio_info` lists the device with its channel and sampling frequency.
- `iio_readdev` records a block of samples that matches the sine (check in Python or with `adcread`-style sequence checks).
- Rate changes through the IIO attribute work while streaming.

## Pitfalls

- The project 05 lessons still apply: xilinx_dma simple mode handles one queued buffer; never terminate an S2MM channel that still holds a buffer.
- The IIO DMA buffer helpers may assume scatter-gather or cyclic DMA; check before committing to them.
- Two drivers for the same hardware: load only one (S40pl must choose).
