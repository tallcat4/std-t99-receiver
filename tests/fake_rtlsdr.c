/* Hardware adapter test double. Never shipped beside the receiver executable. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
typedef struct { uint32_t rate; } device;
static device fake;
static volatile LONG cancelled;
uint32_t rtlsdr_get_device_count(void) { return 1; }
const char* rtlsdr_get_device_name(uint32_t index) { (void)index; return "TEST DOUBLE (no hardware)"; }
int rtlsdr_open(device** out, uint32_t index) { if (index) return -1; *out = &fake; return 0; }
int rtlsdr_close(device* d) { (void)d; return 0; }
int rtlsdr_set_sample_rate(device* d, uint32_t rate) { d->rate = rate; return 0; }
uint32_t rtlsdr_get_sample_rate(device* d) { return d->rate; }
int rtlsdr_set_center_freq(device* d, uint32_t hz) { (void)d; return hz == 146734375 ? 0 : -1;   /* CH18 - 250 kHz */ }
int rtlsdr_set_freq_correction(device* d, int ppm) { (void)d; return ppm == 2 ? 0 : -1; }
int rtlsdr_set_tuner_gain_mode(device* d, int mode) { (void)d; (void)mode; return 0; }
int rtlsdr_set_agc_mode(device* d, int mode) { (void)d; (void)mode; return 0; }
int rtlsdr_reset_buffer(device* d) { (void)d; return 0; }
int rtlsdr_get_tuner_gains(device* d, int* gains) { (void)d; if (gains) { gains[0] = 0; gains[1] = 200; gains[2] = 400; } return 3; }
int rtlsdr_set_tuner_gain(device* d, int gain) { (void)d; return gain == 200 ? 0 : -1; }
int rtlsdr_cancel_async(device* d) {
    (void)d;
    InterlockedExchange(&cancelled, 1);
    return 0;
}
int rtlsdr_read_async(device* d, void (*callback)(unsigned char*, uint32_t, void*),
                      void* context, uint32_t buffers, uint32_t size) {
    (void)d; (void)buffers;
    if (getenv("T98_TEST_USB_FAILURE")) return -7;
    unsigned char* bytes = malloc(size);
    if (!bytes) return -1;
    for (uint32_t i = 0; i < size; ++i) bytes[i] = (unsigned char)(127 + (i * 37 % 5) - 2);
    InterlockedExchange(&cancelled, 0);
    while (!InterlockedCompareExchange(&cancelled, 0, 0)) {
        callback(bytes, size, context);
        Sleep(1);
    }
    free(bytes);
    return 0;
}
