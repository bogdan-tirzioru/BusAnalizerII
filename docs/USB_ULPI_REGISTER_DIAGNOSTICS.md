# USB3300 readback experiment

Branch: `feature/usb-ulpi-diagnostics`, project: `firmBoard735`.

Build and flash the existing STM32CubeIDE project, open the USART console,
leave the Linux USB cable attached, and reset BA2. Save the complete output
from `USB: starting HS/ULPI initialization` through the second `USB DIAG`
block. The extra 100 ms startup delay is for this investigation only.

## Important limitation

The H725/H735 CMSIS register structure marks USB base + 0x034 as reserved.
ST's CubeF7 power-consumption example uses that offset as a ULPI viewport,
but that does NOT establish support on H725/H735. The earlier proposal to
read PHY registers as an ordinary supported H7 HAL operation was too strong.

This branch enables a bounded **experimental** adaptation of that method:
`BA2_ULPI_EXPERIMENTAL_READS=1` in the USER CODE section of `usb_device.c`.
Set it to 0 (or add `-DBA2_ULPI_EXPERIMENTAL_READS=0`) to disable all viewport
accesses and retain only normal controller snapshots. This is investigation
code, not a production PHY driver. Hardware validation is still required.

The viewport address is derived from `USB_OTG_HS`, not the F4/F7 peripheral
base. Only read requests are submitted: no Function/OTG/Scratch writes,
PHY reset, GPIO takeover, or clock changes. Polling has both a 10 ms tick
limit and a 100000-iteration limit if ticks stop. A pre-existing BUSY or the
first timeout aborts all remaining PHY reads; firmware continues. A timed-out
request is not forcibly cleared through undocumented status bits. The
normal USB stack could still be affected by an unsupported/pending viewport
access; use the disabled variant and a power cycle for a baseline comparison.

## Output and interpretation

- `USB DIAG: after USBD_Start`: normal STM32 controller registers before the
  experimental access, plus instantaneous STP/CK/NXT/DIR GPIO input levels.
- `USB PHY: addr=... value=... raw=...`: completed experimental read.
- `USB3300 ID matched twice (0424:0004)`: both reads of addresses 00–03
  matched the expected VID/PID. Only then are control/status reads attempted.
- `BUSY`, `TIMEOUT`, or `ID UNVERIFIED`: readback is inconclusive; this is
  **not proof of a broken PHY, ULPI wiring, or absent clock**. Do not interpret
  the low byte of a timed-out viewport as a PHY value.
- `FUNC/IFACE/OTG/STATUS/DEBUG`: addresses 04, 07, 0A, 13 and 15. Interrupt
  latch 14 is deliberately not read because reading it clears latched events.
  The values are sequential snapshots, not one atomic sample. Function
  Control is decoded into XcvrSelect, TermSelect, OpMode, Reset and SuspendM;
  OTG Control into the DP/DM pulldowns and external VBUS selection.
- `USB DIAG: 100 ms after diagnostics`: controller snapshot after allowing
  more time for host activity. SDIS=0 is a connection request, not proof of
  physical attachment. B-session validity may be forced by software; check
  BVALOEN/BVALOVAL before interpreting it as physical VBUS. GPIO levels are
  instantaneous samples and cannot replace the Rigol trace.

If readback is inconclusive, use the documented controller snapshot and the
existing logic capture rather than adding more undocumented register writes.

Rigol mapping: D0=STP, D1=CLK, D2=NXT, D3=DIR, D4–D7=ULPI DATA0–DATA3.
The diagnostic start timestamp helps locate attempts in the reset capture.
The missing upper data bits still prevent complete command decoding.

## Sources

- ST's original example (F7, not an H7 support guarantee):
  https://github.com/STMicroelectronics/STM32CubeF7/blob/master/Projects/STM32746G-Discovery/Examples/PWR/PWR_CurrentConsumption/Src/stm32f7xx_lp_modes.c
- H735 register layout in this repository:
  `firmBoard735/Drivers/CMSIS/Device/ST/STM32H7xx/Include/stm32h735xx.h`,
  `USB_OTG_GlobalTypeDef::Reserved30`.
- USB3300 register map and field definitions, sections 6.1.4–6.1.6:
  https://ww1.microchip.com/downloads/en/DeviceDoc/00001783C.pdf
