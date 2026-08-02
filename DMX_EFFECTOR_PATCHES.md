# DMX Effector local patches

This fork is the pinned `esp_dmx` dependency for DMX Effector. Local changes
must remain reviewable here and the parent repository must advance its submodule
gitlink to the resulting commit.

## IDF 5.x compatibility and UART2 bring-up fixes

Upstream v4.1 predates ESP-IDF 5.3+ and did not run correctly against the
pinned Arduino-ESP32 (IDF 5.x) toolchain, particularly for UART2 (the DMX
Effector output port). The local patches address this as follows:

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
  result instead of ignoring it.
- **Timer error-path hardening** (`src/dmx/hal/timer.c`). `dmx_timer_init()`
  returned `NULL` from a `bool` function on failure and ignored
  `gptimer_enable()` errors; it now returns `false`, deletes the gptimer on a
  failed enable, tracks a NULL handle, and `dmx_timer_deinit()` guards
  against deinitializing a never-created timer.

## Atomic receive-prefix snapshot

DMX Effector consumes packet metadata and only the short prefix through its
configured input window. Upstream `dmx_receive()` followed by `dmx_read()` does
not make those two operations one frame-coherent transaction: the UART ISR may
reuse the live receive buffer for the next frame before the task-side copy.

The local snapshot APIs register a bounded prefix request. At packet
completion, or immediately before a short packet is reset by the next break,
the ISR retains:

- the requested prefix;
- packet error, start code, size, and RDM classification; and
- the first completed packet until its waiting task consumes it.

An already-complete packet predates request registration and therefore has no
matching retained prefix. The API discards that stale completion and waits for
the next ISR completion rather than reconstructing a result from the live
buffer. Registration also closes the status-check/waiter-registration race by
consuming a coherent snapshot that completed in that interval without waiting
for a notification that could not yet be delivered.

Two public entry points expose that retention:

- `dmx_receive_snapshot()` preserves the original compatibility behavior: it
  reads the driver's current (possibly learned) receive size, then requests an
  atomic prefix snapshot at that size.
- `dmx_receive_num_snapshot()` combines `dmx_receive_num()`'s explicit expected
  length with the same atomic prefix/metadata retention. It lets a consumer
  re-arm a fixed required length on every receive without increasing the prefix
  copied by the ISR. The compatibility wrapper delegates to this entry point
  after reading its learned size.

The explicit-length form is required for short-to-long recovery. The UART ISR
records the observed length when a break terminates a short packet. Reusing the
learned-size wrapper on the next call can therefore keep later full frames
completing at the old short length. DMX Effector now calls
`dmx_receive_num_snapshot()` with its required prefix length on every receive:
8 bytes in legacy mode, 14 bytes in canonical mode, or the reader's bounded
configured prefix. A retained short packet remains `DMX_ERR_NOT_ENOUGH_SLOTS`,
while the first later sufficiently long packet completes at the re-armed
length.

Callers must inspect `packet.err` rather than treating the size return as the
sole received/not-received predicate. A coherently retained start-code-only
short packet has retained size zero and `DMX_ERR_NOT_ENOUGH_SLOTS`; a timeout
also returns zero but reports `DMX_ERR_TIMEOUT`. This distinction lets the
application count the former as coherent raw-driver liveness while continuing
to reject it as application input.

The ISR copies only the requested prefix. Normal DMX Effector firmware requests
8 bytes in legacy mode or 14 bytes in canonical mode; the diagnostic reader
requests its configured logged prefix. The existing `dmx_receive()`,
`dmx_receive_num()`, asynchronous `dmx_read()`, and learned-size
`dmx_receive_snapshot()` behavior remain compatible for other library
consumers.

The retained buffer is fixed storage in each installed driver object, adding
approximately 0.5 KiB per port (including ports used only for TX). This avoids
ISR allocation and caller-buffer lifetime hazards; consumers must budget that
fixed per-port heap cost when selecting how many drivers to install.

`PIO_UNIT_TESTING` exposes `dmx_test_rx_snapshot_retention()` so the exact
retention helper can be exercised without UART hardware. DMX Effector's
receive-path coverage also exercises alternating full-frame signatures and the
short-to-long recovery contract described above.

## Driver install/delete lifecycle fixes

DMX Effector reinstalls drivers routinely: the firmware's RX watchdog restarts
the input driver on a silent line and the TX watchdog restarts the output driver
on repeated incomplete sends. Upstream v4.1 assumes install happens once at
boot, and repeated cycles exposed three lifecycle bugs:

- **UART ISR interrupt-input leak** (`src/dmx/hal/uart.c`).
  `dmx_uart_deinit()` never freed the ISR handle allocated by
  `dmx_uart_init()`, leaking one interrupt input per install/uninstall cycle.
  The leak was introduced upstream in `e316ac99` ("add uart context", the 4.0
  HAL restructure), which dropped the `esp_intr_free()` that `361b16dc` had in
  the 3.x driver-delete path. Repeated restarts could therefore exhaust the
  interrupt inputs, make installation fail with `intr_alloc: No free interrupt
  inputs` (err=261), and leave the application in its input-failure indication.
  Deinit now frees the handle; init defensively frees a stale handle before
  allocating. Both calls check the return value: a failed
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
  When install's own mutex allocation failed, the old path called the general
  `dmx_driver_delete()` against a partially initialized object, took a NULL
  semaphore, and hit a FreeRTOS assert. Merely skipping that semaphore was not
  sufficient: the general delete path could still traverse uninitialized
  parameter state, and install read `driver->mux` after delete freed `driver`.
  The failure branch now clears the global slot and frees only the raw driver
  allocation before returning the logged install failure. General deletion
  remains reserved for objects that reached normal initialization.
- **Lifecycle regression coverage** (`src/dmx/driver.c`, `src/dmx/include/driver.h`).
  A `PIO_UNIT_TESTING`-only one-shot seam forces the next mutex allocation to
  fail without exhausting the device heap; deployment builds contain neither
  its state nor its control function. The DMX Effector lifecycle suite uses the
  seam to check partial-install cleanup, heap integrity and capacity, and later
  recovery, then checks the same heap properties across repeated real
  install/delete cycles.

## Const-correct manufacturer-label registration

`rdm_register_manufacturer_label()` took `char *` while `dmx_driver_install()`
passes the string literal `RDM_MANUFACTURER_LABEL`, producing a
`-Wdiscarded-qualifiers` warning on every build. The parameter is only ever
read: the register path calls `strnlen()` and stores the pointer as a
`DMX_PARAMETER_TYPE_STATIC` parameter whose RDM definition is GET-only
(`.pid_cc = RDM_CC_GET`, no SET handler), so nothing can write through it.
The signature now takes `const char *` (`src/rdm/responder/include/product_info.h`,
`src/rdm/responder/product_info.c`), matching the sibling registration
functions (`rdm_register_device_model_description()`,
`rdm_register_device_label()`), with a commented cast at the
`dmx_parameter_add()` call site whose `void *data` parameter serves both the
copying and pointer-retaining paths. The signature change does not alter runtime
behavior.
