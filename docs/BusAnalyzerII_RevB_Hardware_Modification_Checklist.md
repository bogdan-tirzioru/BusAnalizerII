# BusAnalyzer II Rev B — Hardware Modification Checklist

**Status:** Controlled baseline for the future Rev B respin

**Recorded:** 2026-08-30

**Purpose:** Professional two-channel CAN/CAN FD analyzer and headless Jenkins validation instrument for recording and stimulating an ECU under test.

## Fixed architecture decisions

- Preserve the existing BusAnalyzer II project and its validated firmware work.
- Keep STM32H735ZG in LQFP144.
- Keep two external CAN FD channels.
- Keep USB 2.0 High Speed through USB3300 and ULPI.
- Keep HyperRAM and microSD standalone logging.
- Do not introduce an FPGA.
- Diagnose the existing USB3300 failure before freezing the Rev B USB correction.
- Determine whether the long-capture HyperRAM mismatch is electrical or firmware-related before changing its routing.
- The STM32N6/BGA architecture is a possible future project, not a replacement for BusAnalyzer II.

## A. Required Rev B hardware modifications

### 1. MCU, boot and power

- [ ] Keep STM32H735ZG, LQFP144.
- [ ] Freeze the MCU supply architecture as LDO mode; eliminate ambiguity between LDO and SMPS builds.
- [ ] Connect every unused SMPS-related pin exactly as required for LDO operation.
- [ ] Add test access for MCU 3.3 V, core regulator output, NRST and BOOT0.
- [ ] Keep SWD accessible without removing the PCB from the enclosure.
- [ ] Add a hardware-revision strap so firmware can distinguish Rev B and select the correct board configuration.

### 2. USB3300 and USB High Speed

- [ ] Retain USB3300, subject to the current failure investigation.
- [ ] Redesign/check the entire USB block against the USB3300 hardware checklist.
- [ ] Use a proven 24.000 MHz crystal within the USB3300 frequency tolerance.
- [ ] Calculate the load capacitors from the selected crystal; do not copy 10 pF or 33 pF values without calculation and measurement.
- [ ] Place the crystal and load capacitors immediately beside USB3300 with very short routing.
- [ ] Retain a 12 kOhm ±1% RBIAS resistor with a short ground connection.
- [ ] Implement a deterministic USB3300 RESET circuit with a defined power-up state, valid delay and MCU control.
- [ ] Add short test access for RESET, the 24 MHz oscillator, 60 MHz ULPI CLKOUT, USB3300 supplies and VBUS.
- [ ] Route ULPI signals short, over a continuous ground reference, with similar lengths and no unnecessary vias or long stubs.
- [ ] Route D+/D− as a controlled 90-ohm differential pair over a continuous reference plane.
- [ ] Add low-capacitance USB ESD protection at the connector.
- [ ] Verify USB-C CC resistors, device-mode VBUS sensing and VBUS capacitance.
- [ ] Define the USB shield-to-chassis connection with an EMC-compatible RC/ESD network.
- [ ] Reserve optional source-series resistor footprints for ULPI CLK and critical controls; populate only after signal-integrity measurement.

### 3. Two isolated CAN FD channels

- [ ] Add independent galvanic isolation to CAN1 and CAN2.
- [ ] Use one isolated supply per channel so CAN1 and CAN2 are isolated from logic and from each other.
- [ ] Select transceivers explicitly rated for the required CAN FD nominal/data rates and temperature range.
- [ ] Add low-capacitance automotive CAN TVS protection on CAN_H and CAN_L.
- [ ] Add optional common-mode choke footprints, initially DNP until EMC testing determines whether they help.
- [ ] Provide correct creepage, clearance and isolation slots between logic, CAN1 and CAN2.
- [ ] Connect each DB9 pin 3 to its corresponding isolated channel ground, not USB/logic ground.
- [ ] Provide a controlled shield/chassis connection at each DB9 connector.
- [ ] Add a hardware TX-enable gate per channel, default disabled during reset, so stimulation must be explicitly armed.

### 4. Termination and visible status

- [ ] Replace fixed termination with software-controlled 120-ohm termination on each channel.
- [ ] Use relays or another CAN-FD-compatible, low-parasitic switching method.
- [ ] Default both terminations to OFF after reset.
- [ ] Add termination-state feedback to the MCU.
- [ ] Add per-channel RX/TX activity indication.
- [ ] Add per-channel error/bus-off indication.
- [ ] Add per-channel termination-enabled indication.
- [ ] Add global Power, USB Connected, Ready, Test Running and Pass/Fail indicators.

### 5. Timestamp accuracy

- [ ] Add a specified low-ppm HSE oscillator or TCXO.
- [ ] Drive both FDCAN peripherals from the same timestamp clock source.
- [ ] Provide safe oscillator test access without a harmful clock stub.
- [ ] Establish an oscillator tolerance and temperature budget supporting the guaranteed timestamp specification.
- [ ] Validate hardware timestamp resolution, absolute error and channel-to-channel skew; target 1 microsecond or better.

### 6. HyperRAM

- [ ] Retain the 8 MB HyperRAM architecture.
- [ ] Select one production-orderable part and verify voltage, timing, package and footprint.
- [ ] Add an accessible HyperRAM supply test point.
- [ ] Review and improve local decoupling and supply-plane connection.
- [ ] Review CLK, RWDS, CS and data-bus routing and length relationships.
- [ ] Add optional source-series termination footprints for CLK and, if justified, CS.
- [ ] Do not freeze routing changes until the long-capture mismatch is classified as hardware or firmware.

### 7. microSD logging

- [ ] Retain four-bit SDMMC operation.
- [ ] Add card-detect support.
- [ ] Add low-capacitance ESD protection at the removable card connector.
- [ ] Confirm correct pull-ups and reserve optional SD clock series termination.
- [ ] Make the card accessible without opening the enclosure.
- [ ] Add power-fail detection.
- [ ] Add sufficient hold-up energy to stop logging safely and complete the current filesystem write.

### 8. Jenkins and ECU-fixture connections

- [ ] Add at least one isolated, protected trigger input.
- [ ] Add at least one isolated, protected trigger output.
- [ ] Give both trigger signals a defined inactive state during reset.
- [ ] Add a connector for an external ECU battery/ignition relay fixture.
- [ ] Do not switch high ECU supply current directly on the analyzer PCB.
- [ ] Add a physical Test Start/Logging button for manual operation.
- [ ] Include a hardware watchdog and controlled recovery path.

### 9. Mechanical, EMC and manufacturing

- [ ] Use two robust DB9 connectors with screw locks.
- [ ] Define the PCB outline and connector positions from the selected metal enclosure before final routing.
- [ ] Add enclosure mounting points and a deliberate chassis connection strategy.
- [ ] Keep chassis, USB/logic ground and both isolated CAN grounds deliberately separated.
- [ ] Place ESD components immediately beside their associated connectors.
- [ ] Use a controlled-impedance multilayer PCB; evaluate four versus six layers after isolation placement and routing study.
- [ ] Add manufacturing test pads for every important rail and interface.
- [ ] Add clear channel, connector, polarity, termination and revision markings.

## B. Conditional changes pending current-board diagnosis

- [ ] Final USB crystal and load-capacitor values.
- [ ] Exact USB3300 RESET correction.
- [ ] VBUS-sense correction, if required.
- [ ] ULPI pin/routing correction, if required.
- [ ] D+/D− rerouting, if required.
- [ ] ULPI source-series resistor values, if required.
- [ ] USB3300 replacement with another ULPI PHY, only if the device itself proves unsuitable.
- [ ] HyperRAM routing, power or termination changes arising from the long-capture investigation.

## C. Requirements that do not by themselves require a PCB modification

- Jenkins command-line integration.
- Windows support and WinUSB handling.
- `python-can` API/backend.
- GUI.
- Deterministic scenario upload and replay scheduling.
- Filtering and trigger rules.
- JUnit XML and JSON test results.
- PCAPNG/ASC capture export.
- Formal, versioned USB protocol and performance specification.
- Demonstration of zero-loss 100% simultaneous load on both channels.

## D. Explicit limitations without an FPGA

- Complete physical error-frame waveform capture is not supported.
- Arbitrary, deterministic bit-level error injection is not supported.
- FDCAN error-state capture, TEC/REC/LEC logging, missing-ACK tests, incorrect-bitrate tests, controlled bus-off and higher-level fault scenarios remain supported.

## Release gate before starting the Rev B layout

- [ ] Existing USB3300 enumeration failure has a confirmed root cause.
- [ ] Corrected USB HS path passes repeated cold-start enumeration.
- [ ] USB HS bulk transfer passes an extended stability test.
- [ ] HyperRAM long-capture mismatch is classified and resolved.
- [ ] Existing CAN, RTC/LSE, SDMMC and HyperRAM known-good tests remain reproducible.
- [ ] Every Rev B schematic change is linked to a confirmed defect, a professional product requirement or an explicitly documented optional/DNP provision.
