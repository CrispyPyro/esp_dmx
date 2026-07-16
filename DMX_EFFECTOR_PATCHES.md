# DMX Effector local patches

_Created: 2026-07-15 PDT · Last updated: 2026-07-15 PDT_

This fork is the pinned `esp_dmx` dependency for DMX Effector. Local changes
must remain reviewable here and the parent repository must advance its submodule
gitlink to the resulting commit.

## IDF 5.x compatibility and UART2 bring-up fixes

Upstream v4.1 predates ESP-IDF 5.3+ and did not run correctly against the
pinned Arduino-ESP32 (IDF 5.x) toolchain, particularly for UART2 (the DMX
Effector output port). Two early local commits (`ca44634e`, `f696e64d`, with a
one-line correction in `e04d832d`) address this:

- **UART init modernization** (`src/dmx/hal/uart.c`). On IDF 5.x the init uses
  the native `uart_param_config()` (with `ESP_ERROR_CHECK`) instead of
  upstream's low-level register sequence, keeping the LL sequence for older
  IDF. A `dmx_uart_module_for_num()` helper maps UART number → peripheral
  module: IDF 5.3+ uses the explicit `PERIPH_UARTx_MODULE` enums, older IDF
  falls back to `uart_periph_signal[]`. Hardware flow control is re-disabled
  at the LL layer after config as belt-and-suspenders. The ESP32-C6 LP-UART
  clock selection is retained.
- **UART2 fix: index peripherals by UART number, not DMX port number**
  (`src/dmx/hal/uart.c`, `src/dmx/hal/gpio.c`). Upstream indexed
  `uart_periph_signal[]` and peripheral operations by `dmx_num`; all such
  operations now use `uart->num` so the ISR is allocated on the correct
  interrupt source, and the port-2 context-array guards use `SOC_UART_NUM`
  (the chip's UART count) rather than `DMX_NUM_MAX`. Init also validates the
  UART number against `SOC_UART_NUM` and checks/logs the `esp_intr_alloc()`
  result instead of ignoring it — the diagnostics that later exposed the
  interrupt-input leak below.
- **Timer error-path hardening** (`src/dmx/hal/timer.c`). `dmx_timer_init()`
  returned `NULL` from a `bool` function on failure and ignored
  `gptimer_enable()` errors; it now returns `false`, deletes the gptimer on a
  failed enable, tracks a NULL handle, and `dmx_timer_deinit()` guards
  against deinitializing a never-created timer.

The interim planning artifacts from this work (an IDF 5.5 modernization plan
and an untested-changes scratch file, added in `e04d832d`) were removed again
in `90f4e41f`/`f696e64d`; this tracker is the single authority for local
divergence.

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

## Driver install/delete lifecycle fixes

DMX Effector reinstalls drivers routinely: the firmware's RX watchdog restarts
the input driver on a silent line (settling at roughly one restart per minute
while no console is connected) and the TX watchdog restarts the output driver
on repeated incomplete sends. Upstream v4.1 assumes install happens once at
boot, and repeated cycles exposed three lifecycle bugs, all fixed locally and
all candidates for upstream PRs:

- **UART ISR interrupt-input leak** (`src/dmx/hal/uart.c`).
  `dmx_uart_deinit()` never freed the ISR handle allocated by
  `dmx_uart_init()`, leaking one interrupt input per install/uninstall cycle.
  The leak was introduced upstream in `e316ac99` ("add uart context", the 4.0
  HAL restructure), which dropped the `esp_intr_free()` that `361b16dc` had in
  the 3.x driver-delete path. Hardware-confirmed on S3Gen4 with the camera
  enabled: the second watchdog restart already failed with
  `intr_alloc: No free interrupt inputs` (err=261), permanently red-blinking
  the status LED. Deinit now frees the handle; init defensively frees a stale
  handle before allocating. Both calls check the return value: a failed
  `esp_intr_free()` (e.g. cross-core IPC failure) keeps the handle for a
  later retry instead of orphaning the live allocation, and init aborts so
  the install-retry backoff drives the retry.
- **NON_VOLATILE parameter heap leak** (`src/dmx/driver.c`).
  `dmx_driver_delete()` freed only `DMX_PARAMETER_TYPE_DYNAMIC` parameter
  data, but `dmx_parameter_add()` also mallocs for `NON_VOLATILE` (and its
  `NON_VOLATILE_STAGED` variant), and `dmx_driver_install()` registers such
  parameters unconditionally (identify device, device label, DMX personality,
  DMX start address). Roughly four small heap blocks (~100 B with allocator
  overhead) leaked per install/delete cycle. Delete now frees every
  malloc-backed type; `STATIC` (caller-owned) and `NULL` remain untouched.
- **NULL-mutex assert in delete** (`src/dmx/driver.c`).
  When install's own mutex allocation fails it calls back into
  `dmx_driver_delete()`, which took the NULL semaphore and hit a FreeRTOS
  assert — turning heap exhaustion into a panic instead of a logged install
  failure. Delete now takes and deletes the mutex only when it exists.

Validation: the interrupt leak fix is hardware-confirmed on S3Gen4 (restarts
proceed past #2 with no `intr_alloc` errors). The parameter-leak fix is
source-verified; positive bench confirmation would log
`esp_get_free_heap_size()` across a dozen forced restarts and confirm free
heap stays flat. The NULL-mutex path requires simulated heap exhaustion and is
source-verified only.

## Considered and deferred

- **Skipping RDM parameter registration for non-RDM consumers.** The
  registrations are inert in this pinned version: `rdm_send_response()` is the
  only path that transmits a response and neither the library's receive path
  nor DMX Effector calls it, so the parameters cost only a small, bounded,
  correctly-freed allocation per install. Bypassing registration would fork
  `dmx_driver_install()` semantics for negligible gain; a compile-time
  responder-strip seam belongs upstream. Caveat for future rebases: if a newer
  esp_dmx auto-responds inside `dmx_receive()` (as 3.x-era designs did), the
  DMX Effector input port (`tx=-1`, `en=-1`) cannot physically transmit a
  response and this decision must be revisited.
