# BA2 Board #2 — USB HS rework and diagnostic experiment (2026-10-13)

## Scope and status

Hardware: **BusAnalizerII (BA2), physical board #2**, USB3300 external ULPI PHY and STM32 USB HS controller. Test host: Linux `ghita-desktop`, xHCI. Experiments occurred 13 October 2026.

**Key achievement:** board #2 successfully enumerated as STM32 CDC ACM at **USB high speed (480 Mbit/s)**, including on more than one host port. Enumeration is **intermittent**, and a bidirectional CDC echo has **not** yet been demonstrated.

**Hardware preservation decision:** **Do not rework board #2 further** unless a specific hardware defect is established with evidence. In particular, leave the currently fitted **33 pF** crystal load capacitors in place for now.

## Rework completed before testing

| Item | Action |
|---|---|
| USB3300 | PHY completely replaced |
| 24 MHz crystal | Replaced with **ECS-240-18-33-JEN-TR3** |
| Crystal load capacitors | Replaced with **33 pF** (both sides) |
| USB connector | Micro-USB receptacle replaced |
| Clock source | **Crystal directly** drives the PHY; **no STM32 PA8 clock reroute** on board #2 |

These changes were made together, so today's results do **not** isolate which replacement resolved the original complete failure.

For comparison, board #1 had previously required an experimentally routed 24 MHz clock from PA8 and had not achieved this successful enumeration.

## Linux observations and evidence

### Successful enumeration on port 1-13

```text
[30215.060939] usb 1-13: new high-speed USB device number 13 using xhci_hcd
[30215.277695] usb 1-13: New USB device found, idVendor=0483, idProduct=5740, bcdDevice= 2.00
[30215.277704] usb 1-13: Product: STM32 Virtual ComPort
[30215.277707] usb 1-13: Manufacturer: STMicroelectronics
[30215.321786] cdc_acm 1-13:1.0: ttyACM0: USB ACM device
```

### Successful enumeration on port 1-12

```text
[30474.560237] usb 1-12: new high-speed USB device number 29 using xhci_hcd
[30474.776788] usb 1-12: New USB device found, idVendor=0483, idProduct=5740, bcdDevice= 2.00
[30474.776796] usb 1-12: Product: STM32 Virtual ComPort
[30474.802836] cdc_acm 1-12:1.0: ttyACM0: USB ACM device
```

Later, another successful attempt on port 1-12:

```text
[31565.292943] usb 1-12: new high-speed USB device number 50 using xhci_hcd
[31565.509624] usb 1-12: New USB device found, idVendor=0483, idProduct=5740, bcdDevice= 2.00
[31565.509633] usb 1-12: Product: STM32 Virtual ComPort
[31565.535678] cdc_acm 1-12:1.0: ttyACM0: USB ACM device
```

`lsusb -t` confirms the device's **CDC Communications interface** and **CDC Data interface** each at `480M`:

```text
Port 012: Dev 050, If 0, Class=Communications, Driver=cdc_acm, 480M
Port 012: Dev 050, If 1, Class=CDC Data, Driver=cdc_acm, 480M
```

`ls -l /dev/ttyACM0` returned `crw-rw---- 1 root dialout 166, 0 ... /dev/ttyACM0`.

**Interpretation:** the USB3300 oscillator, ULPI path, HS signaling, control endpoint and CDC enumeration are capable of working. `480M` is the **negotiated link speed**, not a measured transfer throughput or long-term reliability claim.

### Failed enumeration (intermittent)

Repeated failed attempts occurred on **both ports 1-13 and 1-12**, including:

```text
device descriptor read/64, error -71
Device not responding to setup address.
device not accepting address ..., error -71
unable to enumerate USB device
```

Linux `-71` is `EPROTO` (protocol error). A later successful enumeration does not establish that the underlying failure is resolved. Other devices in the larger log (GenesysLogic hub, CH341 serial adapter, SEGGER J-Link, Logitech receiver) are **not** the BA2 CDC device.

### Physical debugger connection / reset behavior

- Initial failures were observed with the debugging hardware present.
- The successful device-50 enumeration was observed after **physically removing the STAG/JTAG/SWD debugger connector**, not merely closing a debug program.
- Later report: **with the debugger physically disconnected and USB cable already attached to the PC**, holding the STM32 RESET button for approximately **2 seconds** and releasing it produced repeated successful enumerations.
- The number of attempts and success fraction were not recorded; a controlled short-reset/long-reset comparison has not yet been performed.
- Consequently, whether the debugger's electrical connection itself causes failures remains **unproven**.
- MCU NRST, USB3300 RESETB, software reset through ULPI, and USB logical disconnect/reconnect are distinct reset mechanisms; their actual interconnection and behavior still require verification.

## CDC communication attempts

After device 50 enumerated, the host showed `ttyACM0` and `480M`.

1. **Minicom:** displayed **Disconnected**. Nevertheless, `lsusb -t` still showed device 50 at `480M`, and `sudo fuser -v /dev/ttyACM0` showed a `minicom` process holding the device. This did **not** establish a physical USB disconnect.
2. `sudo stty -F /dev/ttyACM0 115200 raw -echo -crtscts` completed without error.
3. `echo "hello BA2" | sudo tee /dev/ttyACM0` completed and printed `hello BA2` locally; that is **tee output**, **not** a response from STM32.
4. `cat /dev/ttyACM0` yielded no returned bytes.
5. During the later echo-test investigation, a debugger breakpoint in **`CDC_Receive_HS()` did not hit**, according to the test report. This must be interpreted cautiously because debugger attachment itself is one experimental variable.

**Result:** host device open/write succeeded without a reported error; **STM32 reception, IN return transfer and end-to-end echo have not been confirmed**.

## Firmware branch work on 2026-10-13

- Reference/parent branch: `feature/usb-ulpi-diagnostics`.
- New branch: **`feature/usb-cdc-echo-test`**, created from the reference branch.
- Commit `7d70d4c`: updated `firmBoard735/USB_DEVICE/App/usbd_cdc_if.c` to echo CDC RX data back on the IN endpoint, rearming OUT after the transmit-complete callback.
- Commit `634caf3`: added `docs/USB_CDC_ECHO_TEST.md` with host-side test instructions.
- **Status: unverified on hardware; reported not working yet.** No claim of tested firmware success or compilation validation.
- Code examination indicated that `HAL_PCD_DataOutStageCallback()` forwards data to `USBD_LL_DataOutStage()`, and the USB HS IRQ is enabled. The diagnostic firmware also calls `USB_DiagReconnect()` and uses a forced disconnect / 100 ms delay / reconnect. USB class setup, actual OUT endpoint arming and full receive callback execution remain to be verified dynamically.
- The unmodified parent branch's `CDC_Receive_HS()` simply rearmed reception and did **not** echo data.

## Crystal load discussion — hypothesis, not defect confirmation

The fitted ECS crystal has **18 pF specified load capacitance**; PCB plus USB3300 pin parasitics are not yet measured. With equal 33 pF external capacitors, the simple series contribution is 16.5 pF, plus effective parasitics. **27 pF** was discussed as a possible *future experiment*, **not** as a verified corrective value or confirmed cause of `-71`. Current decision: **retain 33 pF** and do not modify hardware without stronger evidence.

## Open questions / next non-invasive experiments

1. With **no debugger attached**, repeat short press (~0.1–0.2 s) versus long press (~2 s) of NRST, with USB cable left attached; log each trial and `sudo dmesg -w` output. Control cable, power and host port.
2. For the *already flashed* echo-test firmware, verify CDC receive/response with a single program that writes bytes and reads them back; do not mistake shell `tee` output for returned data.
3. Confirm the exact flashed firmware binary/commit and whether the running target is `firmBoard735`; code changes were made to that target, not `FirmwareB`.
4. To debug without disrupting enumeration, prefer counters or buffered UART diagnostics; breakpoints on USB interrupts may disrupt timing. Trace `CDC_Init_HS`, `HAL_PCD_DataOutStageCallback`, `USBD_CDC_DataOut`, `CDC_Receive_HS` and IN transmit completion.
5. Inspect USB controller/PHY software startup ordering, ULPI identity/readiness diagnostics and the forced disconnect/reconnect behavior. Distinguish STM32 NRST from PHY RESETB.
6. Once reliable echo works, characterize repeated enumeration, sustained transfer integrity, error rates and throughput at USB HS. No hardware changes unless measurements justify them.

## Summary

**Confirmed:** reworked board #2 can enumerate as `0483:5740`, Linux `cdc_acm`, `/dev/ttyACM0`, **480M**. **Unresolved:** intermittent `-71` on initial enumeration and lack of proven end-to-end CDC echo. **Constraint:** preserve current board hardware.
