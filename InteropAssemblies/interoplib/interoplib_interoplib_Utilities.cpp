//-----------------------------------------------------------------------------
//
// LEDTREES: adapter from the Utilities stubs to the ledtrees_sysinfo component.
//
// Identification, CRC, the SD bus probe, the reset reason, the heap watermark
// and handing out the core dump all live in a separate IDF component - the
// ledtrees-idf-components repository, components/ledtrees_sysinfo. What is left
// here is unwrapping the managed arrays and validating their sizes.
//
// The EXCEPTION is NativeWifiReconnect: it depends on the network module of
// nf-interpreter itself (NF_ESP32_IsToConnect from NF_ESP32_Network.h), so it is
// not moved into the component and its implementation stays here in full.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_Utilities.h"

#include <esp_wifi.h>
#include <ledtrees_sysinfo.h>

// NF_ESP32_IsToConnect is the auto-reconnect flag of the network module
// (NF_ESP32_Wireless.cpp): while it is set, the WIFI_EVENT_STA_DISCONNECTED
// handler calls esp_wifi_connect on its own - see NativeWifiReconnect. Declared
// through the header rather than a local extern, since a local copy could drift
// away from the real type.
#include <NF_ESP32_Network.h>

using namespace interoplib::interoplib;

// A forced Wi-Fi STA reconnect, the cure for a "dead link" the driver does not
// notice: the node believes it is still connected to the PREVIOUS instance of
// the AP (MAIN rebooted faster than the beacon timeout, or that timeout never
// fired) and keeps sending frames with stale keys into nowhere. The managed
// watchdog (ScreenCommandService) calls this method when MAIN goes quiet.
//
// esp_wifi_disconnect plus an unconditional esp_wifi_connect. A disconnect
// alone is not enough: the driver only raises STA_DISCONNECTED (handled in
// targetHAL_Network.cpp, which calls esp_wifi_connect itself while
// NF_ESP32_IsToConnect is set) from the connected or connecting state. In idle -
// where the auto-reconnect chain has broken, since the result of
// esp_wifi_connect in the handler is not checked and a single collision with a
// scan breaks the chain for good - a disconnect produces no event, and without a
// direct connect the node would stay offline until the power is cycled. The
// direct call is harmless in the other states too: connected or connecting
// returns an error, and after a disconnect the event handler does the reconnect.
// The flag is set explicitly rather than relying on its current value: the call
// only makes sense on an STA device, where a connection is expected in normal
// operation. Errors are not raised through hr: retrying in any driver state is
// harmless, and the watchdog will repeat on its own interval anyway.
void Utilities::NativeWifiReconnect( HRESULT &hr )
{
    hr = S_OK;

    // Gate on the mode: a reconnect only makes sense where there is an STA
    // interface. On an AP device (MAIN, or the unconfigured setup mode) the call
    // is a no-op: esp_wifi_disconnect would return an error there anyway, but
    // what matters is leaving NF_ESP32_IsToConnect alone, since for an AP
    // configuration the flag must stay false. If Wi-Fi is not initialised,
    // esp_wifi_get_mode returns an error, which is also a no-op.
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA))
    {
        return;
    }

    // The flag is set explicitly rather than relying on its current value: on an
    // STA device it is already true (set by the normal connect with AutoConnect),
    // so the assignment is idempotent, but it guards a call made in the window
    // before that connect has happened.
    NF_ESP32_IsToConnect = true;
    esp_wifi_disconnect();
    esp_wifi_connect();
}

void Utilities::NativeGetBaseMac( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr )
{
    // managed must pass a buffer of at least 6 bytes: anything shorter, or NULL,
    // would be a write past the end
    if (param0.GetBuffer() == NULL || param0.GetSize() < 6) {
        hr = CLR_E_INVALID_PARAMETER;
        return;
    }

    lt_sys_base_mac(param0.GetBuffer());
}

// An incremental zlib compatible CRC32: a table driven byte loop on nanoCLR
// takes tens of seconds to checksum a megabyte-sized .ltf, the ROM function
// takes milliseconds.
unsigned int Utilities::NativeCrc32( unsigned int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, signed int param3, HRESULT &hr )
{
    const uint8_t *data = (const uint8_t *)param1.GetBuffer();
    signed int offset = param2;
    signed int count = param3;

    // The bounds of the managed array are checked here: the CLR bounds check does
    // not apply to native code. offset and count are added as unsigned - for
    // signed int the sum of two large positive values overflows, which is
    // undefined behaviour and lets the compiler drop the check altogether.
    if (data == NULL || offset < 0 || count < 0 ||
        (uint32_t)offset + (uint32_t)count > param1.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return param0;
    }

    return lt_sys_crc32(param0, data + offset, (size_t)count);
}

// A diagnostic probe of the SD bus (see Storage.Init on the managed side):
// 0 ok, 1 timeout (no response), 2 CRC or data, 3 anything else.
signed int Utilities::NativeSdProbe( uint8_t width, uint16_t freqKhz, CLR_RT_TypedArray_UINT8 pins, HRESULT &hr )
{
    hr = S_OK;

    if (pins.GetSize() < 6) {
        hr = CLR_E_INVALID_PARAMETER;
        return LT_SD_OTHER;
    }

    signed int result = lt_sys_sd_probe(width, freqKhz, (const uint8_t *)pins.GetBuffer());

    if (result == LT_SD_PARAM) {
        hr = CLR_E_INVALID_PARAMETER;
        return LT_SD_OTHER;
    }

    return result;
}

// The reason for the last reset, the raw esp_reset_reason_t value (0 unknown,
// 1 poweron, 2 external, 3 software, 4 panic, 5 int wdt, 6 task wdt, 7 wdt,
// 8 deep sleep, 9 brownout, 10 sdio). The managed side repeats exactly these
// numbers (TelemetryEventCodes.ResetReason): a translation table here would only
// add one more place for the two enumerations to drift apart when IDF is
// updated.
uint8_t Utilities::NativeGetResetReason( HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_reset_reason();
}

// Watermark: the minimum free memory over the whole uptime. The current "free"
// answers whether there is enough right now, this one answers whether we ever
// came close to the edge.
unsigned int Utilities::NativeGetMinFreeHeap( bool spiRam, HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_min_free_heap(spiRam);
}

// ---------------------------------------------------------------------------
// The core dump of a native panic (the coredump partition, docs/telemetry.md).
//
// The managed log ring (LogRing) does not survive a panic: by then the CLR is no
// longer executing, and the stack of the task that crashed is visible ONLY from
// here.
// ---------------------------------------------------------------------------

unsigned int Utilities::NativeGetCoredumpSize( HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_coredump_size();
}

signed int Utilities::NativeReadCoredump( unsigned int offset, CLR_RT_TypedArray_UINT8 buffer, signed int count, HRESULT &hr )
{
    hr = S_OK;

    // The bounds of the managed array are checked here: the CLR bounds check does
    // not apply to native code (the same reasoning as in NativeCrc32).
    if (buffer.GetBuffer() == NULL || count < 0 || (uint32_t)count > buffer.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return 0;
    }

    return lt_sys_coredump_read(offset, buffer.GetBuffer(), (size_t)count);
}

bool Utilities::NativeEraseCoredump( HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_coredump_erase();
}
