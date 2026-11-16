# Diagnostic tools

The normal firmware deliberately omits continuous audio-path instrumentation.
Diagnostic sampling and DEBUG telemetry consume CPU time and can affect the
real-time workload being measured, so enable these packages only while
investigating a specific problem and remove them afterwards.

Start with the symptom-led checks in [Troubleshooting](TROUBLESHOOTING.md).
Home Assistant's temporary
[Assist WAV recording](INSTALLATION.md#temporarily-record-the-audio-received-by-assist)
is usually the least intrusive way to inspect the processed speech delivered by
the board.

## Wi-Fi and Sendspin playback diagnostics

These entities are always available (they do not require a diagnostic package)
and are declared in this order in the firmware. They describe the network
attachment and the Sendspin playback pipeline:

| Entity | Type | Meaning |
|---|---|---|
| WiFi SSID | text sensor | The Wi-Fi network the device is currently joined to. |
| WiFi Signal | sensor | Received signal strength in dBm; values closer to 0 are stronger. |
| IPv4 Address | text sensor | The device's current IP address. |
| Sendspin Track Artist | text sensor | Artist of the stream the Sendspin server is serving. |
| Sendspin Track Title | text sensor | Title of the stream the Sendspin server is serving. |
| Sendspin Track Progress | sensor | Playback position in whole seconds. |
| Sendspin Last Write | sensor | Duration of the most recent audio write, in milliseconds. |
| Sendspin Longest Write | sensor | Longest audio write observed, in milliseconds. |
| Sendspin Recovery Restarts | sensor | Times the Sendspin client was restarted after losing its server connection. |
| Sendspin Short Writes | sensor | Writes that accepted fewer bytes than requested. |
| Sendspin Stalled Writes | sensor | Writes that took 500 ms or longer. |
| Sendspin Write Calls | sensor | Total audio write calls since boot. |
| Sendspin Zero Writes | sensor | Writes that carried zero bytes. |
| Sendspin Buffer Full | sensor | Current Sendspin buffer fill, as a percentage. |
| Sendspin Buffer Backlog | sensor | Buffered audio still awaiting playback, in milliseconds. |
| Sendspin Buffer Backlog Bytes | sensor | Bytes buffered at the most recent write. |
| Sendspin Largest Buffer Backlog Bytes | sensor | Highest buffer backlog ever observed, in bytes. |

`Sendspin Largest Buffer Backlog Bytes` is the high-water mark of the receive
buffer. When it sits pinned at its capacity while playback is expected, the
consumer side of the pipeline is not draining — the buffer fills and audio
stalls. Rising `Sendspin Short Writes`/`Sendspin Zero Writes` and a climbing
`Sendspin Recovery Restarts` counter point at the Sendspin connection flapping.

## Raw TDM levels

Use [`diagnostics/tdm-levels.yaml`](../diagnostics/tdm-levels.yaml) when you need
to verify the physical input slots before AFE processing. It reports:

| TDM slot | Signal |
|---|---|
| 0 | First physical microphone |
| 1 | Analog playback reference used by AEC |
| 2 | Second physical microphone |
| 3 | Unused channel / noise floor |

For a local checkout, add it as a second package:

```yaml
packages:
  tdm_diagnostics: !include diagnostics/tdm-levels.yaml
```

If the diagnostic file is not stored beside your editable configuration, it
can instead be included as a remote Git package pinned to the same release tag.

The entities expose RMS levels for checking that both microphones respond,
that playback reaches the reference slot and that the unused slot remains near
the noise floor. `disabled_by_default` only hides an entity in Home Assistant;
it does not stop the device from sampling it. Remove the package when the test
is complete to eliminate its periodic RMS work.

## AFE runtime diagnostics

Use [`diagnostics/afe-runtime.yaml`](../diagnostics/afe-runtime.yaml) for
intermittently low, metallic or discontinuous processed audio, or when a voice
interaction appears to starve the processing pipeline. It publishes:

- AFE input and output levels;
- output misses;
- input and output ring drops;
- feed rejections;
- fetch timeouts;
- short-interval performance telemetry in the logs.

For a local checkout:

```yaml
packages:
  afe_runtime_diagnostics: !include diagnostics/afe-runtime.yaml
```

If the diagnostic file is not stored beside your editable configuration, it
can instead be included as a remote Git package pinned to the same release tag.

A small non-zero startup value is not necessarily a fault. Record whether a
counter **increases during the failing interaction**. Healthy operation should
leave the error counters unchanged after initialization.

Remove the package after diagnosis because its DEBUG logging and telemetry add
work to the real-time audio path.

## Playback resource comparisons

Before changing the playback architecture, include these packages in the local
wrapper that already contains your device name and Wi-Fi secrets:

```yaml
packages:
  firmware: !include waveshare-va.yaml
  resource_diagnostics: !include diagnostics/resources.yaml
  cpu_diagnostics: !include diagnostics/cpu.yaml
```

`resources.yaml` reports internal 8-bit RAM and PSRAM separately: current free
bytes, allocator minimum free bytes since boot, and largest contiguous free
block. It also reports the maximum ESPHome loop interval observed in each
five-second reporting window. Loop interval is a responsiveness measurement,
not CPU utilization. The allocator minima are sums of each matching heap
region's low-water mark; they need not represent one simultaneous allocation.
Five-second sampling can miss brief changes in the largest block.

`cpu.yaml` additionally enables FreeRTOS runtime accounting. It reports each
core's busy percentage over the preceding sample interval using idle-task
runtime, and logs task stack minimum-free bytes once a minute or when the
**Resource task stack snapshot** button is pressed. The first CPU measurement
needs two samples. Busy percentage is an idle-time estimate, not attribution of
CPU time to individual tasks or interrupts. Do not interpret loop time or CPU
frequency as utilization. Stack minima are lifetime high-water marks, not the
current stack use.

CPU profiling snapshots briefly suspend scheduling and scan task stacks. The
profiler allocates space for up to 64 task records in PSRAM; failure produces a
warning rather than a fabricated measurement. Runtime accounting also has
overhead. Use identical packages, ESPHome/ESP-IDF versions, logger level,
network settings, wake-word models, volume, and audio files for both builds.
Keep the more intrusive AFE/TDM packages off for the initial resource comparison.
After the comparison, remove these packages and repeat listening tests without
profiling to check for instrumentation effects.

Capture separate logs for these workloads, allowing startup to settle first:

| Workload | Suggested capture |
|---|---|
| Connected idle with wake word enabled | 2 minutes |
| Continuous HTTP music | 3 minutes |
| Music plus local chimes or ringing | 2 minutes |
| Music plus repeated Assist requests and TTS replies | 3 minutes |
| Stopped playback, then repeated start/stop transitions | 2 minutes |

For Sendspin, also capture connected idle with the server available, continuous
Sendspin music, and Sendspin music with Assist/TTS. Exercise HTTP/Sendspin
switching and server/Wi-Fi reconnection, and note whether other group members
are playing. Sendspin adds its own buffer and tasks and requests Wi-Fi driver
tuning, so HTTP-only measurements do not establish its resource margins.

Reboot before independent runs if comparing lifetime heap/stack minima. Use
the same sequence for a transition run and check whether memory recovers after
playback stops. Record glitches, resets, Wi-Fi/API reconnects, and task snapshot
warnings alongside the metrics. A successful compile alone does not validate
audio behavior or runtime headroom.

For serial logging, use your actual device port (Linux example):

```sh
esphome logs waveshare-va.local.yaml --device /dev/serial/by-id/YOUR_DEVICE > capture.log 2>&1
python3 scripts/analyze_resources.py capture.log > capture-summary.json
```

Stop logging with Ctrl-C. The analyzer accepts complete `[resources:...]` log
rows and reports sample count, minimum, median, nearest-rank p95, and maximum,
plus observed task stack minima. Keep raw logs locally: other firmware log
lines can contain recognized speech, media URLs, or network information.

Preserve the baseline YAML and firmware binary before migrating. For the first
migration, set HTTP buffers explicitly to the old player's values; compare
architecture with equivalent buffers before attempting memory tuning.

## SEGGER SystemView tracing

Use [`diagnostics/systemview.yaml`](../diagnostics/systemview.yaml) to record a
FreeRTOS task and interrupt trace for [SEGGER
SystemView](https://www.segger.com/products/development-tools/systemview/).
This is the right tool when a workload needs task-level or interrupt-level
attribution that the CPU busy percentages from `cpu.yaml` cannot provide, such
as pinning down which task starves the audio pipeline or the AFE feed.

Add it as a second package in your local wrapper:

```yaml
packages:
  firmware: !include waveshare-va.yaml
  systemview: !include diagnostics/systemview.yaml
```

The package re-includes the `app_trace` IDF component that ESPHome excludes by
default and routes trace data through JTAG into the ESP32-S3 TRAX trace memory.
The board's USB port is the built-in USB-Serial/JTAG controller, so no extra
wiring is needed; UART0 is not exposed on the USB connector, which is why this
uses the JTAG destination rather than a UART one.

SystemView hooks run on every task switch and ISR entry/exit, so build a
dedicated tracing firmware and remove the package for normal listening tests.
Flash the build to the board, then capture with Espressif's OpenOCD:

```sh
openocd -f board/esp32s3-builtin.cfg
```

In a second terminal, connect to OpenOCD and start the trace. The ESP32-S3 is
dual-core, so capture both CPU files:

```sh
telnet 127.0.0.1 4444
> esp sysview start file://pro-cpu.svdat file://app-cpu.svdat
```

Reproduce the workload, then stop the trace:

```sh
> esp sysview stop
```

Load each `.svdat` file into SystemView. To see both cores in one view, use the
free Eclipse [Impulse](https://toem.de/index.php/products/impulse) plugin and
add both files to a multi-adapter port. ESP-IDF uses its own SystemView
FreeRTOS event IDs, so replace
`$SYSVIEW_INSTALL_DIR/Description/SYSVIEW_FreeRTOS.txt` with
`$IDF_PATH/tools/esp_app_trace/SYSVIEW_FreeRTOS.txt` before opening a trace.

## Reporting results

Record the installed firmware version, whether the build is precompiled or
source-built, the exact interaction and the counter values immediately before
and after it. When relevant, include a privacy-reviewed Assist WAV and logs
from before the first symptom through recovery.

Never publish Wi-Fi credentials, API keys, tokens or private media URLs.
