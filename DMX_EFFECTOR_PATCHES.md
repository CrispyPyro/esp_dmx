# DMX Effector local patches

_Created: 2026-07-15 PDT · Last updated: 2026-07-15 PDT_

This fork is the pinned `esp_dmx` dependency for DMX Effector. Local changes
must remain reviewable here and the parent repository must advance its submodule
gitlink to the resulting commit.

## Atomic receive-prefix snapshot

DMX Effector consumes packet metadata and only the short prefix through its
configured input window. Upstream `dmx_receive()` followed by `dmx_read()` does
not make those two operations one frame-coherent transaction: the UART ISR may
reuse the live receive buffer for the next frame before the task-side copy.

The local `dmx_receive_snapshot()` API registers a bounded prefix request. At
packet completion, or immediately before a short packet is reset by the next
break, the ISR retains:

- the requested prefix;
- packet error, start code, size, and RDM classification; and
- the first completed packet until its waiting task consumes it.

An already-complete packet predates request registration and therefore has no
matching retained prefix. The API discards that stale completion and waits for
the next ISR completion rather than reconstructing a result from the live
buffer. Registration also closes the status-check/waiter-registration race by
consuming a coherent snapshot that completed in that interval without waiting
for a notification that could not yet be delivered.

The ISR copies only the requested prefix. Normal DMX Effector firmware requests
8 bytes in legacy mode or 14 bytes in canonical mode; the diagnostic reader
requests its configured logged prefix. The existing `dmx_receive()`,
`dmx_receive_num()`, and asynchronous `dmx_read()` APIs are unchanged for other
library consumers.

The retained buffer is fixed storage in each installed driver object, adding
approximately 0.5 KiB per port (including ports used only for TX). This avoids
ISR allocation and caller-buffer lifetime hazards; target builds and C3/S3 HIL
must still confirm acceptable heap headroom and driver installation.

`PIO_UNIT_TESTING` exposes `dmx_test_rx_snapshot_retention()` so the exact
retention helper can be tested without UART hardware. Physical validation must
still confirm that continuous full-universe input causes no RX errors, FIFO
overruns, or restarts on both ESP32-C3 and ESP32-S3 targets.
