# GnuChanSoundRecord

A GTK 3 desktop recorder for Linux. It captures mono microphone audio through
the PulseAudio client interface (including PipeWire's PulseAudio-compatible
server), writes PCM WAV itself, and uses FFmpeg only for codecs that are
actually present. Missing encoders are omitted from the format list; choosing
an unavailable format never silently produces a different format.

## Features

- Select an available microphone source; monitor sources are excluded.
- Start, pause, resume, and stop recording. Paused audio is discarded and does
  not count toward the displayed duration.
- Record to WAV, MP3, OGG/Vorbis, or FLAC when the matching encoder is detected.
- Select 44.1/48/96 kHz and PCM 16/24/32-bit WAV depth. FLAC offers 16/24-bit;
  PCM depth is disabled and labeled not applicable for MP3/OGG.
- Adjust software microphone gain (0–200%). This does not change the system's
  microphone volume; clipping is clamped.
- Show a live input level and waveform, and a waveform preview for saved files.
- Play, rename, and delete recordings. New names include a date/time stamp.
- Choose an output folder. The selected folder and sample rate/depth are saved
  along with format, quality, and gain in
  `~/.config/GnuChanSoundRecord/config.conf` (or `$XDG_CONFIG_HOME`).
- Keyboard shortcuts: `Ctrl+R` record, `Space` pause/resume (or play when idle),
  `Ctrl+S` stop, `Ctrl+P` play/stop, `Ctrl+O` choose folder, `F2` rename,
  `Delete` delete, and `Escape` stop an active recording.

## Dependencies and format availability

Runtime dependencies for Debian-based distro package lists:

- `libgtk-3-0` and `libglib2.0-0` for the GTK/GIO desktop application.
- `pulseaudio-utils` (`pactl`, `parec`) for microphone discovery and capture.
- A running PulseAudio-compatible server, provided by `pulseaudio` or
  `pipewire-pulse`. The latter also requires a working PipeWire audio session.
- Optional playback: `pulseaudio-utils` provides `paplay` for WAV; `ffmpeg`
  provides `ffplay` for compressed files.
- Optional encoding: `ffmpeg` with `libmp3lame`, `libvorbis`, and/or FLAC
  encoder support enables the corresponding MP3, OGG, and FLAC formats.

The required build dependencies on Debian/Ubuntu are:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libglib2.0-dev
```

`libgtk-3-dev` and `libglib2.0-dev` provide the development headers and
pkg-config metadata; they are not required as `-dev` packages at runtime.
Recording and microphone selection need both `pactl` and `parec` from
`pulseaudio-utils`, plus the compatible server described above.

```sh
sudo apt install pulseaudio-utils
```

WAV/PCM recording is built in. MP3 requires an FFmpeg build with the
`libmp3lame` encoder; OGG requires `libvorbis`; FLAC requires FFmpeg's `flac`
encoder. FFmpeg is optional: without it, only WAV is offered. WAV playback
uses `paplay` when installed; compressed playback uses `ffplay`.

Run `./build/GnuChanSoundRecord --check-dependencies` for the detected runtime
commands and encoders. This diagnostic intentionally returns success even if
optional codecs or audio-server programs are absent, since WAV remains a valid
build/runtime mode.

## Build, test, install

```sh
make
make check
./build/GnuChanSoundRecord
sudo make install
```

`make check` runs the built-in format/WAV-header self-test, reports runtime
dependency availability, and stages the install layout under `build/` without
installing to the system. The noninteractive installer is
`sudo ./install.sh install`; it installs the executable and desktop entry.
For package staging, set `DESTDIR` (no root needed), e.g.
`DESTDIR=debian/tmp PREFIX=/usr ./install.sh install`. The executable is
available as both `/usr/local/bin/GnuChanSoundRecord` and
`/usr/local/bin/gnuchan-sound-record`. Override `PREFIX` and/or `DESTDIR` for
packaging. Use `sudo ./install.sh uninstall` (or `sudo make uninstall`) to
remove both command names and the desktop entry.

## Capture and quality details

The recorder asks `parec` for the selected PCM rate and depth, then applies the
software gain while writing a standard mono PCM WAV stream. MP3/OGG/FLAC are
encoded from that completed PCM file by FFmpeg when recording stops. Lossy
quality presets map to 128/192/320 kb/s for MP3 and Vorbis quality 3/5/8; FLAC
compression levels are 0/5/8. Codec availability is checked by encoder name,
not inferred from a filename or extension.

Pause suspends the capture subprocess and drains/discards any already-buffered
input, so captured duration excludes paused audio. The live meter and waveform
are derived from microphone samples and are not simulated. A waveform for an
existing compressed recording requires FFmpeg; uncompressed WAV previews are
read directly when FFmpeg is unavailable.

This project is Linux/X11/GTK 3 desktop software. Audio capture requires the
session's PulseAudio protocol endpoint; a direct PipeWire-only session without
the PulseAudio compatibility server is not supported.
