# External USB3300 module: PC7 reset firmware

## Target and wiring

This experiment targets the reworked BA2 board with its original USB3300
removed and the external Waveshare USB3300 USB HS Board wired to ULPI.
Base: feature/usb-cdc-diagnostics-cleanup, commit 21fd0fad.
Project: firmBoard735. Branch: feature/usb-external-phy-reset-pc7.

The supplied PCB screenshot identifies via 33 connected to STM32 LQFP144
pin 94, PC7. The BA2 schematic also maps pin 94 to PC7. This is separate
from PA8/MCO at MCU pin 97 and from the regulator output test point TP2.

| Connection | Function |
|---|---|
| PC7 / MCU pin 94 / via 33 | External module RESET, CN1 pin 8 |
| Module USB VBUS | BA2 5 V regulator input; this path powered BA2 |
| BA2 regulated 3.3 V | Must also supply module 3.3 V, CN1 pins 19/20 |
| Module GND / BA2 GND | Common ground, CN1 pins 17/18 |
| Module CLKOUT / CN1 pin 10 | STM32 PA5, 60 MHz ULPI clock input |

The module's header 5V net is a host-side supply input, separated from USB
VBUS by a power switch. It did not power BA2; using actual VBUS did.
The return 3.3 V connection still needs confirmation on hardware.
The brief power-up after soldering was not a meaningful enumeration test.

## Firmware behavior

- MX_GPIO_Init preloads PC7 HIGH before enabling its push-pull output,
  holding the active-high PHY RESET throughout peripheral initialization.
- Immediately before USBD_Init, the firmware asserts RESET and holds PC0
  (ULPI STP) HIGH for startup protection and wake-up from suspend.
- It waits 10 ms for the powered module's crystal/PLL startup or recovery.
- It drives RESET LOW and keeps other PHY inputs stable for another 1 ms.
- Normal HAL_PCD initialization then configures STP and the other ULPI pins
  for their alternate functions.
- The old PA8 24 MHz MCO setup is removed in both C and CubeMX configuration;
  this module uses its own 24 MHz crystal. PLL settings are unchanged.
- Existing CDC echo and logical disconnect/reconnect behavior are retained.

The delay assumes valid module power and a working crystal. It does not
measure CLKOUT or guarantee it exists: verify 60 MHz at PA5 with the scope.
USB3300 requires CLKOUT to be active when hardware RESET is released.
The RESET pin resets the transceiver, not the ULPI register bank; a full
3.3 V power cycle is needed to restore the register bank to POR defaults.
A reset pulse cannot compensate for absent power or broken ULPI wiring.

## First hardware test

1. With power off, verify the PC7-to-RESET connection and shared ground.
2. Confirm BA2 3.3 V reaches the external module, then verify its supply.
3. Build and flash this branch. Power-cycle for a fresh PHY register state.
4. Capture RESET, CLKOUT and STP during cold startup with the scope.
   Expected: PC7 high through peripheral init and the 10 ms startup guard,
   then low; 60 MHz must be active before the falling RESET edge.
   STP stays high through reset release and at least the 1 ms guard.
   Retain the USART startup log. Expected additional lines:

   ~~~
   USB PHY: PC7 RESET asserted, STP high; startup guard 10 ms
   USB PHY: PC7 RESET released low; recovery guard 1 ms complete
   ~~~

5. Observe host enumeration and CDC echo separately. No successful reset
   waveform, enumeration or echo test is claimed until measured.

## Build validation

The firmBoard735/Debug target was clean-built on ghita-desktop with
STM32CubeIDE 1.11.0 and its GNU Arm toolchain on 30 October 2026.
Result: 0 errors, 3 warnings in unchanged fatfs_test.c and sd_test.h.
The firmware has not been flashed or tested on hardware by this change.

## Sources

- BA2 schematic: https://github.com/bogdan-tirzioru/BusAnalizerII/blob/master/schematic.PDF
- Waveshare schematic: https://www.waveshare.com/w/upload/9/9e/USB3300-USB-HS-Board-Schematic.pdf
- USB3300 datasheet, sections 6.1.9.2, 6.1.9.3, 6.1.11 and 6.4.2:
  https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/00001783C.pdf
