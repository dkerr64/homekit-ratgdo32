#pragma once

#include <cstdint>

// Keep the electrical sequence independent of the ESP32 driver so the known
// inverted-UART pull regression and the shutdown pin state can be tested.
namespace gdo_uart
{
constexpr unsigned long SEC1_BAUD = 1200;
// 8E1 occupies 11 bit periods (9.17 ms). Leave scheduling margin for its echo.
constexpr unsigned long SEC1_ECHO_TIMEOUT_MS = 20;

template <class Pins>
__attribute__((always_inline)) inline void quiet_tx(Pins &pins, int tx)
{
    // Preload before switching the mux: a stale HIGH latch would assert the bus.
    pins.preload_low(tx);
    pins.select_gpio(tx);
    pins.route_gpio(tx);
    pins.disable_tx_pullup(tx);
    pins.enable_tx_pulldown(tx);
    pins.enable_output(tx);
}

template <class SerialPort, class Pins>
bool begin_sec1(SerialPort &serial, Pins &pins, int rx, int tx, uint32_t format)
{
    quiet_tx(pins, tx);
    if (serial)
    {
        // A repeated begin() with unchanged parameters may reuse the existing
        // driver without restoring its pin mux after quiet_tx().
        serial.end();
        quiet_tx(pins, tx);
    }
    serial.begin(SEC1_BAUD, format, rx, tx, true);
    // begin() may overwrite the input pulls for an inverted UART (upstream #192).
    // Apply the board's RX bias afterwards, even if the framework changes defaults.
    const bool rx_ready = pins.disable_rx_pulldown(rx) && pins.enable_rx_pullup(rx);
    if (!serial || !rx_ready || !serial.setRxFIFOFull(1))
    {
        serial.end();
        quiet_tx(pins, tx);
        return false;
    }
    serial.setTimeout(SEC1_ECHO_TIMEOUT_MS);
    return true;
}
}
