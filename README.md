# BusAnalyzer II (BA2)

STM32-based two-channel CAN/CAN FD analyzer for bus capture, standalone logging
and automated ECU testing. BA2 builds on the first BusAnalyzer project's capture
work, adding external HyperRAM, microSD and a USB 2.0 High-Speed ULPI interface.

Status: prototype hardware and firmware bring-up. The current firmware implements
dual-channel CAN FD acquisition and storage diagnostics. USB enumeration remains
under investigation; lossless simultaneous full-load acquisition, continuous
logging and the complete host interface are not yet validated product features.

This overview follows guitarAmp's documentation structure: requirements, selected
hardware, routing, current firmware, open decisions and bring-up. Source baseline:
`master` commit `90d8e84`, reviewed on 2026-10-02. Hardware observations below are
reported bring-up results, not measurements repeated for this documentation.

## Agreed requirements

| Area | Requirement / intended result |
| --- | --- |
| CAN interfaces | Two external CAN FD channels, with classic CAN support. |
| Acquisition | Zero-loss capture at 100% simultaneous load on both channels; validate payload integrity as well as frame counts. |
| Timing | Target timestamp resolution of 1 µs or better; establish accuracy, rollover handling and channel-to-channel alignment. |
| Memory | Internal SRAM queue followed by HyperRAM for longer captures. |
| Storage | microSD for standalone logging and stored test scenarios. |
| USB | USB 2.0 High Speed for host control and captured data; document protocol and sustained throughput. |
| Host workflow | Linux-first operation, command-line automation and Jenkins integration. |
| ECU stimulation | Controlled CAN transmission and scenario replay are planned. |
| Isolation | Galvanic isolation for operator protection in a future hardware revision; define the isolation barrier and isolated supplies. |
| Product extensions | Configurable termination, status LEDs, filtering/triggers, host API/GUI and EMC/ESD provisions remain future work. |

Windows and `python-can` support appear in the Rev B checklist as product
extensions. They are not an implemented BA2 host stack. A Jenkins firmware build
also does not establish that an ECU test station can use BA2.

## Selected MCU and digital hardware

The active [CubeMX project](firmBoard735/firmBoard735.ioc) targets
**STM32H735ZGT6, LQFP144**. One prototype was reworked to STM32H725ZG; this
repository's active build remains the H735 project. Check device-specific
peripherals, startup and linker configuration before using it on the H725 board.

| Function | Current allocation / implementation |
| --- | --- |
| CPU | Cortex-M7; current CubeMX configuration reports 550 MHz. |
| MCU supply | LDO hardware; firmware selects `PWR_LDO_SUPPLY`. |
| CAN channel 1 | FDCAN1, PA11 RX / PA12 TX; PA10 labelled `FD1_STBM`. |
| CAN channel 2 | FDCAN3, PF6 RX / PF7 TX; PF8 labelled `FD2_STBM`. Logical channel 2 is not FDCAN2. |
| CAN clocks | PLL2Q at 80 MHz, shared by the two configured controllers. |
| External RAM | OCTOSPI1 HyperBus; capture code targets fitted S27KL0641, 8 MiB. |
| Removable storage | SDMMC1 in 4-bit mode with FatFs. |
| Calendar clock | RTC with 32.768 kHz LSE. |
| USB | USB OTG HS device controller with external USB3300 ULPI PHY; current class is CDC. |
| Diagnostics | USART1 on PB14 TX / PB15 RX, 115200 8N1; DMA1 Stream0 for TX. |
| Indicators | LED0 PE4 and LED1 PE3. |
| Debug | SWD/J-Link; remote-server helper in `scripts/`. |

Earlier MCU/internal-PHY alternatives are investigations for possible future
hardware. They do not change the checked-in H735/USB3300 baseline.

### MCU supply and recovery

The prototypes are wired for LDO operation. An earlier firmware selection of
SMPS prevented normal debug access. Reported recovery was BOOT0 high, full erase
through J-Link SWD at 100 kHz, BOOT0 low, then reflash firmware selecting
`HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY)`. Preserve that supply selection when
regenerating CubeMX code. A full erase destroys the existing firmware.

## Capture and data routing

| Path | Current routing |
| --- | --- |
| CAN 1 | Transceiver → FDCAN1 RX FIFO0 → shared SRAM capture queue |
| CAN 2 | Transceiver → FDCAN3 RX FIFO0 → shared SRAM capture queue, with channel flag |
| Storage | SRAM queue → main-loop batch processing → HyperRAM |
| Verification | Stop both controllers at the capture threshold → drain queued frames → read back HyperRAM and check records |
| Diagnostics | Runtime counters and verification results → queued USART1 DMA console |
| Future host data | Internal capture records → explicit wire serializer → USB host transport |
| Future standalone log | Captured records → file writer → microSD; not established by the existing FatFs tests |

### Current CAN configuration

Both controllers start in bus-monitoring/listen-only mode with CAN FD and BRS
enabled. At the configured 80 MHz kernel clock:

| Parameter | Channel 1 and channel 2 |
| --- | --- |
| Nominal bitrate | 1 Mbit/s: prescaler 5, TSEG1 13, TSEG2 2, SJW 2 |
| Data bitrate | 5 Mbit/s: prescaler 1, TSEG1 12, TSEG2 3, SJW 3 |
| Nominal/data sample point | 87.5% / 81.25% |
| RX FIFO0 | 64 elements per controller, 64 data bytes per element |
| Message RAM offsets | FDCAN1: 0; FDCAN3: 1152 words |
| TX buffers/queue | Zero in the current startup configuration |

The sniffer records standard/extended ID, RTR, FD, BRS, ESI, DLC and channel
identity. Internal records contain a raw 16-bit FDCAN timestamp. This is not yet
the protocol's 64-bit microsecond time value; conversion, rollover extension and
cross-channel synchronization need validation before promising timestamp accuracy.

## Memory and HyperRAM capture

The internal capture queue holds **4096 × 72-byte records = 288 KiB**, including
space for 64-byte CAN FD payloads. It resides in AXI SRAM (`RAM_D1`). Console DMA
storage uses a dedicated `RAM_D2` section; HyperRAM scratch batches use DTCM and
polling OSPI transfers. The linker also reserves a CAN TX section in `RAM_D3`;
that reservation alone does not implement transmission.

HyperRAM capture reserves the first 4 KiB for future metadata. Records are
written in batches of 32, with a 10 ms partial-batch flush interval. Verification
reads batches of 31 to exercise different transaction boundaries.

The current application is a capture regression workflow: after at least
**100,000 stored frames**, it stops both FDCAN controllers, drains the queued
records and performs readback. It is not yet an indefinitely running logger.
Verification expects a known external generator pattern:

- Standard CAN ID `0x100` and a little-endian sequence counter in bytes 0–3.
- Classic CAN: DLC 8, remaining bytes `AA 55 12 34`.
- CAN FD: DLC 15 / 64 bytes, bytes 4–63 equal `(counter + byte index) & 0xff`.

Use that traffic pattern when interpreting sequence/payload verification results.
Arbitrary real-bus traffic is not expected to satisfy these diagnostic checks.
Reported high-load corruption and long-capture mismatches still require a
reproducible resolution; memory identification and short tests do not prove
lossless full-load recording.

## USB3300 and USB High-Speed investigation

The current implementation initializes a CDC device through the external ULPI
PHY. Startup logs `USBD_Init`, class/interface registration and `USBD_Start`.
These return values establish software initialization, not host enumeration.
The code explicitly closes the PC2_C/PC3_C analog switches for ULPI DIR/NXT.

Reported prototype observations:

- The original 24 MHz crystal circuit was unstable.
- Feeding USB3300 XI from MCU PA8 produced continuous approximately 60 MHz CLKOUT.
- Short ULPI activity was observed, but successful enumeration was not established.
- USB3300 RESET was tied to ground on the PCB; MCU-controlled reset was proposed.
- Board 1 was instrumented for logic-analyzer probing. Board 2 rework was planned
  around PHY replacement, connector replacement and an active oscillator.

These observations do not identify a confirmed root cause. Next checks are
deterministic PHY reset, clock stability, decoded ULPI reads/writes, connector and
D+/D− continuity, VBUS detection and repeated cold-start enumeration. Then
validate actual HS negotiation and sustained transfer throughput.

## RTC and microSD

RTC/LSE and an 8 GB microSD/FatFs test were reported working on the first board.
The application runs RTC, HyperRAM, SD and FatFs diagnostics before starting CAN
capture. The FatFs read/write test creates or overwrites
`BusAnalyzerII_FatFs_Long_Filename_Test.txt` on the card. Use a test card during
bring-up. These checks do not implement continuous CAN logging, replay or
power-fail-safe file handling.

## Binary host protocol

See [Binary Host Protocol v0.1](docs/BINARY_PROTOCOL_V0_1.md) for framing,
command IDs and wire record fields. The protocol is transport-independent,
little-endian and serialized explicitly rather than copying C structures.

The source includes information/status, RTC and CAN-configuration handlers,
including classic CAN, FD without BRS and FD with BRS configuration. Capture
control IDs remain reserved and return `NOT_SUPPORTED`. USB receive/worker
integration and capture-stream serialization remain work to complete.

An optional `BAII_PROTOCOL_SELFTEST_ON_BOOT` switch is documented in
[`main.h`](firmBoard735/Core/Inc/main.h); it is disabled by default. Protocol
self-tests do not validate the USB physical path.

## Serial logging and startup

Use `Console_Write` and `Console_Printf` from
[`console.h`](firmBoard735/Core/Inc/console.h) for queued logging. The console
has 64 message slots of 320 bytes and reports dropped messages and UART errors.
`Console_Flush` waits for transmission completion and belongs in normal/main
context. Startup deliberately flushes diagnostics before acquisition; runtime
statistics use queued output.

The main loop services the sniffer and HyperRAM capture and prints statistics
approximately once per second: channel frame/error/FIFO counters, SRAM queue
occupancy/high-water mark, drops, HyperRAM failures/wraps and console health.
Use these together with sequence/payload checks to assess integrity.

## Build, Jenkins and remote debug

Import `firmBoard735/` into STM32CubeIDE and build its **Debug** configuration.
Use [`firmBoard735.ioc`](firmBoard735/firmBoard735.ioc) for peripheral generation;
preserve custom modules, linker sections and `USER CODE` blocks when regenerating.
The separate `FirmwareB/` projects are not the Jenkins build target.

The root [Jenkinsfile](Jenkinsfile) checks out `GIT_REF` (default `*/master`),
imports `firmBoard735` with STM32CubeIDE 1.11.0 headless tooling, builds Debug,
then records ELF size and SHA-256. It archives firmware outputs, build logs and
commit identification. Tool paths and checkout credentials are installation
specific. This pipeline builds firmware; it does not flash or exercise hardware.

[`scripts/jlink-remote.sh`](scripts/jlink-remote.sh) manages a J-Link GDB server
using machine-specific tool/probe settings. Review those settings before use on
another host or board.

## Future hardware and open decisions

The [Rev B hardware checklist](docs/BusAnalyzerII_RevB_Hardware_Modification_Checklist.md)
records the earlier respin proposal, including isolation, independently supplied
CAN channels, controllable termination, protected fixture triggers and EMC
provisions. It is a planning document, not evidence that those features exist on
the prototypes or that a respin has been authorized.

The later isolated-transceiver discussion identified ADM3055E as a candidate
with integrated isolated power. Final part selection, data-rate target (5 or
8 Mbit/s), barrier requirements and PCB implementation remain to be frozen.
The present firmware baseline is 5 Mbit/s data rate; 8 Mbit/s is not a validated
BA2 capability.

The MCU-only design does not provide arbitrary deterministic bit-level error
injection or complete physical error-waveform capture. Controller error counters
and protocol-state diagnostics are separate capabilities; scope any future
error-generation requirement accordingly.

## Bring-up and validation plan

1. Preserve the known LDO supply setup and verify rails, boot and debug access.
2. Confirm the USB enumeration failure's root cause and validate the correction
   across repeated cold starts before selecting a respin change.
3. Reproduce single-channel and simultaneous dual-channel CAN FD captures using
   the documented generator pattern; record exact bitrates, load and firmware SHA.
4. Resolve payload/sequence mismatches, FIFO losses and queue drops, then repeat
   the 100,000-frame HyperRAM readback at increasing loads through 100%.
5. Validate timestamp units, rollover and channel alignment against an external
   reference before claiming ≤1 µs timing.
6. Complete host transport, serialization and capture control after USB works;
   measure sustained throughput with both channels active.
7. Add continuous microSD logging and controlled TX/replay in separate stages,
   followed by hardware-backed Jenkins test scenarios.
8. Reconcile isolation and other product requirements with the Rev B checklist
   before freezing any new schematic or layout.

## Repository contents and references

- [`firmBoard735/`](firmBoard735/): active H735 firmware, CubeMX and CubeIDE project.
- [`FirmwareB/`](FirmwareB/): additional firmware projects outside the current CI target.
- [`layout/`](layout/): PCB design files.
- [`schematic.PDF`](schematic.PDF): checked-in schematic.
- [`docs/`](docs/): host protocol and future hardware checklist.
- [`AN/`](AN/): reference material.
- [`samplescode/`](samplescode/): USB3300 reference/sample code, not the active application.
- [`scripts/`](scripts/): remote debug helper and related files.
- [`Jenkinsfile`](Jenkinsfile): firmware build pipeline.

Update this overview as implementation and measured results change. A configured
peripheral or passing firmware build is not hardware validation.
