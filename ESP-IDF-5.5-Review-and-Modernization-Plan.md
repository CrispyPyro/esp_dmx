# ESP-IDF 5.5.x / Arduino-ESP32 3.3.4 Review & Modernization Plan (esp_dmx)

This document captures:

1. The current state of this repository relative to ESP-IDF 5.5.x and Arduino-ESP32 3.3.4.
2. Findings from a code review (UART/timer/GPIO HAL) and from Espressif docs via Context7.
3. A conservative, minimal-risk plan to modernize fragile areas **without changing the timing model** (important for DMX downstream device compatibility).

> Scope constraints provided by the requester:
>
> - Target hardware: **ESP32-S3**
> - Arduino framework: **3.3.4** (built on ESP-IDF **5.5.x**)
> - Library already “works” in the current device configuration, so changes must be low risk
> - **RDM support is not required for the requester’s scenario**, but it is acceptable to leave RDM code intact
> - Prior UART2-related workarounds are no longer desired

---

## Repository status (local workspace notes)

At the time of writing:

- `src/dmx/hal/uart.c` has local, uncommitted modifications.
- There is an untracked `newuart.c` in the repository root which appears to be an experimental copy of `uart.c`.

**Recommendation:** before implementing any functional patches, either delete `newuart.c` or move it into a scratch path and/or add it to `.gitignore` to prevent accidental shipping.

---

## Architecture overview (what the code is doing today)

### UART: custom ISR + LL FIFO access (not the high-level UART driver)

The DMX/RDM implementation is built around:

- A **custom UART ISR** allocated via `esp_intr_alloc()` using `uart_periph_signal[dmx_num].irq`.
- Reading/writing UART FIFOs via low-level functions:
  - `uart_ll_read_rxfifo()`
  - `uart_ll_write_txfifo()`
- Configuring UART parameters via `uart_ll_*` functions.
- DMX break generation by **inverting TX** for `break_len` via a timer-driven state machine.

This design is consistent with the needs of DMX/RDM (tight timing), but it relies on ESP-IDF internal layers (HAL/LL and SoC mappings) which are less stable than the public UART driver APIs.

### Timer: ESP-IDF 5.x uses GPTimer

For ESP-IDF v5+, the code uses `driver/gptimer.h` and installs an alarm callback.

The callback returns a boolean which is then used by the driver to request a yield (typical ISR pattern).

### GPIO: sniffer uses GPIO ISR service

The sniffer uses `gpio_isr_handler_add()` and relies on the application to call `gpio_install_isr_service()`.

---

## Findings from ESP-IDF 5.5.x docs (Context7)

### 1) RS485 half duplex mode is supported, but via UART driver

ESP-IDF’s documented approach for RS485 half duplex is:

```c
ESP_ERROR_CHECK(uart_set_mode(uart_num, UART_MODE_RS485_HALF_DUPLEX));
```

However, the docs note `uart_set_mode()` must be called **after** `uart_driver_install()`.

This library does **not** call `uart_driver_install()` today.

Implication:

- If we want to adopt the “official” RS485 mode (auto-RTS direction control), it likely requires switching to the UART driver or partially using it.
- This is a non-trivial refactor and could affect timing, so it is not the best first step for a conservative modernization.

### 2) Private headers are unstable

ESP-IDF explicitly warns:

- Headers in paths containing `esp_private` are **private APIs**.
- Private APIs may change or be removed even in minor/patch releases.

This library currently includes:

- `esp_private/esp_clk.h`
- `esp_private/periph_ctrl.h`

Implication:

- These includes are a fragility vector for Arduino-ESP32 3.3.4 (and future 3.3.x) because Arduino tracks ESP-IDF point releases.

---

## Key code-level issues found

### Issue A — RTS “get” and “set” mismatch (likely correctness bug)

Current behavior:

- `dmx_uart_get_rts()` reads the **software RTS level** (`sw_rts` bit).
- `dmx_uart_set_rts()` calls `uart_ll_set_rts_active_level()` which configures the **polarity/active level**, not the current `sw_rts` output.

Implication:

- Direction control can be inconsistent or dependent on previous config.
- It may contribute to “works on some targets/configs, flaky on others” behavior.

This is a strong candidate for a **minimal-risk fix** because it corrects behavior without changing the state machine timing.

### Issue B — `esp_private/*` usage (fragility)

Even if the code currently builds, relying on `esp_private/*` is discouraged for long-term stability.

### Issue C — Target-specific UART2 / ESP32-C6 LP-UART handling (not needed)

There is code specifically handling UART2 on ESP32-C6 (LP UART) and other UART2-related branching.

The requester no longer wants UART2-specific workarounds.

For ESP32-S3 specifically, keeping code generic (UART0/1) and avoiding special casing reduces fragility.

---

## Background from the requester (UART2 context)

Previously attempted scenario:

- ESP32-S3 hardware
- Two UARTs were in use (one input, one output)
- Desired mapping:
  - input on UART1
  - output on UART2 (to avoid UART0 interfering with USB logging)
- UART2 caused panics/reboot loops; workarounds caused timing issues; downstream device stopped responding.

Working interim mapping:

- input on UART0
- output on UART1
- USB logging works

This strongly suggests we should prefer changes that **do not alter UART ISR timing** or TX break/MAB behavior.

---

## Proposed plan (minimal-risk modernization)

### Goal

Keep the existing “custom ISR + FIFO” design (timing-sensitive), but reduce fragility and fix correctness issues.

### Non-goals

- Large refactor to `uart_driver_install()` / UART driver event queue.
- Changing DMX timing generation strategy (break/MAB state machine).
- Removing RDM code (can remain as-is).

### Step 1 — Clean workspace before patching

1. Decide what to do with `newuart.c`:
   - delete it, or
   - move it to a scratch folder, or
   - add to `.gitignore`.
2. Decide whether to:
   - revert `src/dmx/hal/uart.c` to `HEAD` and apply fresh patches, or
   - incorporate existing local diffs.

### Step 2 — Fix RTS control implementation

Update `dmx_uart_set_rts()` so it sets the same underlying state that `dmx_uart_get_rts()` reads.

Expected outcome:

- Deterministic driver direction control (DE/RE) when switching between read and write.
- Reduced chance of subtle direction glitches.

### Step 3 — Reduce private-header usage

Goal: avoid `esp_private/esp_clk.h` and `esp_private/periph_ctrl.h` if possible.

Approach:

- Prefer public headers (where available) for peripheral enable/reset.
- If enable/reset is not strictly required on Arduino-ESP32 3.3.4 (often UART is already clocked), consider gating it behind a config define to keep default behavior stable.

### Step 4 — Remove UART2/C6-specific workarounds

Remove or isolate:

- ESP32-C6 LP UART clock selection changes.
- Other UART2-specific workarounds that are not required for ESP32-S3.

This reduces code paths that are likely to break across SoCs/IDF versions.

### Step 5 — Validation checklist

Because the risk is “downstream device stops responding”, validation should be practical:

1. **Arduino**:
   - Build and run `examples/Arduino_DMXWrite` (or the device’s real sketch).
2. **ESP-IDF**:
   - Build `examples/ESPIDF_DMXWrite` under ESP-IDF 5.5.x.
3. Runtime checks:
   - Continuous DMX output with correct baud (250k) and stable framing.
   - No panics/reboot loops.
   - Confirm DMX bus direction toggles correctly.

---

## Optional future track (higher risk): adopt UART driver RS485 mode

ESP-IDF’s recommended RS485 half-duplex approach is driver-based:

- `uart_driver_install()`
- `uart_param_config()`
- `uart_set_pin()`
- `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`

Benefits:

- Uses stable public APIs.
- Hardware/driver-managed RTS direction control.

Risks:

- Timing and buffering behavior changes could affect DMX/RDM responsiveness.

Recommendation:

- Only attempt this behind a compile-time option or on a separate branch once the minimal-risk fixes are proven stable.
