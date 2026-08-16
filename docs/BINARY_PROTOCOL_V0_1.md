# BusAnalyzerII Binary Host Protocol v0.1

## Scope

Protocol v0.1 defines the transport-independent binary format between BusAnalyzerII firmware and the PC. USB HS will carry these byte messages later; the protocol itself does not depend on USB packet boundaries.

All multibyte integers are **little-endian**. C structures are never copied directly to the wire.

## Common message header

Every message begins with a fixed 20-byte header.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic: ASCII `BAII` |
| 4 | 1 | Protocol major: `0` |
| 5 | 1 | Protocol minor: `1` |
| 6 | 1 | Message type |
| 7 | 1 | Flags |
| 8 | 4 | Transaction ID |
| 12 | 4 | Sequence number |
| 16 | 4 | Payload length |

Message types:

- `0x01` command
- `0x02` response
- `0x03` asynchronous event
- `0x10` CAN data

The response echoes the command transaction ID. Device-generated messages use a monotonically increasing sequence number so the PC can detect lost protocol messages.

## Command and response payloads

A command payload starts with:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Command ID |
| 2 | 2 | Reserved, write zero |

A response payload starts with:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Command ID |
| 2 | 2 | Status |

Status values:

- `0x0000` OK
- `0x0001` bad length
- `0x0002` unknown command
- `0x0003` invalid parameter
- `0x0004` busy
- `0x0005` HAL error
- `0x0006` not supported
- `0x0007` internal error

## Implemented commands

### `0x0001 GET_INFO`

No command-specific request bytes.

Successful response after the 4-byte response prefix:

| Size | Field |
|---:|---|
| 1 | firmware major |
| 1 | firmware minor |
| 2 | firmware patch |
| 4 | capability flags |
| 4 | FDCAN kernel clock in Hz |
| 4 | HyperRAM size in bytes |
| 4 | STM32 device ID |
| 1 | CAN channel count |
| 1 | RTC valid/readable |
| 2 | reserved |

Current firmware reports an FDCAN kernel clock of 96 MHz and 8 MiB HyperRAM.

### `0x0002 GET_STATUS`

No command-specific request bytes.

Successful response contains nine `uint32_t` values:

1. uptime in ms
2. CAN RX frames
3. SRAM buffered frames
4. SRAM dropped frames
5. FDCAN FIFO lost events
6. HyperRAM stored frames
7. HyperRAM write errors
8. HyperRAM lost frames
9. HyperRAM wrap count

### `0x0010 GET_RTC_TIME`

No command-specific request bytes.

Successful response contains one `uint64_t unix_time_us` after the response prefix.

The analyzer clock is UTC. Timezone conversion belongs on the PC.

### `0x0011 SET_RTC_TIME`

Request after the 4-byte command prefix:

- `uint64_t unix_time_us`

The current STM32 RTC setter applies whole-second calendar time. The response returns the time read back from the RTC.

Accepted calendar range is 2000-01-01 through 2099-12-31 UTC.

### `0x0020 GET_CAN_CONFIG`

Request after the command prefix:

| Size | Field |
|---:|---|
| 1 | channel: 1 or 2 |
| 3 | reserved |

Successful response after the response prefix:

| Size | Field |
|---:|---|
| 1 | channel |
| 1 | mode: 0 normal, 1 listen-only |
| 1 | frame format |
| 1 | reserved |
| 4 | FDCAN kernel clock Hz |
| 4 | nominal bitrate |
| 4 | data bitrate |
| 2 | nominal sample point, per-mille |
| 2 | data sample point, per-mille |
| 2 | nominal prescaler |
| 2 | nominal TSEG1 |
| 2 | nominal TSEG2 |
| 2 | nominal SJW |

### `0x0021 SET_CAN_CONFIG`

Request after the command prefix:

| Size | Field |
|---:|---|
| 1 | channel: 1 or 2 |
| 1 | mode: 0 normal, 1 listen-only |
| 1 | frame format |
| 1 | reserved |
| 4 | nominal bitrate |
| 4 | data bitrate |
| 2 | nominal sample point, per-mille; 0 means 87.5% |
| 2 | data sample point, per-mille |

v0.1 accepts **classic CAN only** (`frame_format = 0`, data bitrate/sample point = 0). The firmware calculates an exact nominal bitrate from the 96 MHz FDCAN clock and selects the closest requested sample point, preferring timing near 16 time quanta.

The successful response returns the full applied CAN configuration, including prescaler, TSEG1, TSEG2 and SJW.

Changing CAN configuration stops, deinitializes, reinitializes and restarts the selected FDCAN peripheral. Channel 1 then restores the unrestricted sniffer global filter and FDCAN timestamp counter.

## Reserved capture commands

The following IDs are allocated but return `NOT_SUPPORTED` in v0.1:

- `0x0030 CAPTURE_START`
- `0x0031 CAPTURE_STOP`
- `0x0032 CAPTURE_CLEAR`
- `0x0033 GET_CAPTURE_STATUS`

They are reserved now so PC software can keep stable command IDs as capture control is added.

## CAN wire record

CAN records are variable length and already allow up to 64 data bytes even though the current acquisition path is still classic CAN.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | timestamp in microseconds |
| 8 | 4 | CAN ID |
| 12 | 2 | flags |
| 14 | 1 | channel |
| 15 | 1 | DLC |
| 16 | 1 | actual data length |
| 17 | 1 | reserved |
| 18 | N | data, 0..64 bytes |

Flags:

- bit 0: extended ID
- bit 1: RTR
- bit 2: CAN FD
- bit 3: BRS
- bit 4: ESI
- bit 5: TX direction
- bit 6: error/event record

The current `CAN_SnifferFrame` SRAM/HyperRAM structure is intentionally unchanged. A later serializer will translate captured internal records into this host wire record without coupling the storage layout to the protocol.

## Transport rule

Do not execute heavy command handlers directly from a USB interrupt callback. The future USB receive callback should enqueue complete control messages and let the main/worker context call `BAII_Protocol_HandleMessage()`.
