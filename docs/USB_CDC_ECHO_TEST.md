# BA2 USB HS CDC echo diagnostic

Branch: `feature/usb-cdc-echo-test` (forked from `feature/usb-ulpi-diagnostics`).

## Scope
Modifies `firmBoard735/USB_DEVICE/App/usbd_cdc_if.c` only (plus this document).
Received CDC OUT data is sent back unmodified on the CDC IN endpoint. The OUT
endpoint is not rearmed until the transmit-complete callback, preventing
reuse of the RX buffer during an active transmit. The receive path rearms
after empty packets or a failed transmit attempt.

This is a simple, single-buffer diagnostic echo, not a production throughput
implementation. No timeout/retry for failed IN completions is included.

## Test on Linux
Rebuild and flash firmware from this branch. Disconnect SWD debugger when
comparing with the successful board-2 USB enumeration baseline.

Check `lsusb -t` for `480M` and `/dev/ttyACM0`.

Close minicom or other serial applications first. Run:

```bash
sudo fuser -v /dev/ttyACM0
python3 - <<'PY'
import os, select, termios
fd = os.open('/dev/ttyACM0', os.O_RDWR | os.O_NOCTTY)
old = termios.tcgetattr(fd)
try:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[3] = 0
    attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)) | termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    payload = b'BA2 USB HS echo test\r\n'
    os.write(fd, payload)
    ready, _, _ = select.select([fd], [], [], 3)
    received = os.read(fd, len(payload)) if ready else b''
    print('Sent:', repr(payload))
    print('Received:', repr(received))
    print('PASS' if received == payload else 'FAIL/TIMEOUT')
finally:
    termios.tcsetattr(fd, termios.TCSANOW, old)
    os.close(fd)
PY
```

Use `sudo dmesg -w` to watch for USB disconnects and errors. No echo should
be expected from the unmodified parent branch.
