# utbd — UTC Time Broadcast Daemon

utbd answers SIP calls — through any SIP PBX or provider, or
directly — and plays a time
signal in the audio format of the NIST shortwave time stations
**WWV** and **WWVH**: second ticks, minute and hour markers, standard
tones, the 100 Hz BCD time code and a spoken time announcement.

**Service:** [time.tgabor.eu](https://time.tgabor.eu)  
**Contact:** `info@tgabor.eu`  
**Version:** 2.0.0

This is an independent service and is not operated by or affiliated
with NIST.  It reproduces the broadcast *format* only; its station
identification is its own.

---

## How It Works

```
Telephone ─── PSTN/VoIP ─── SIP PBX / provider ─── SIP + RTP ─── utbd
                                                                   │
                                                         WWV/WWVH signal generator
                                                         (synthesised from UTC)
```

utbd registers with a SIP PBX or provider and answers every call
routed to it, sending G.711 (A-law or µ-law) RTP.  The signal is computed sample by sample from the system
clock, so all callers hear the same broadcast; up to `max_calls`
callers are served at once.  The server's clock should be disciplined
by NTP or GPS.

---

## Broadcast Format

As described in NIST Special Publication 250-67, *NIST Time and
Frequency Radio Stations: WWV, WWVH, and WWVB*.

| Element | WWV | WWVH |
|---------|-----|------|
| Second ticks | 5 ms of 1000 Hz | 5 ms of 1200 Hz |
| Protected zone | 10 ms silence before, 25 ms after each tick | same |
| Omitted ticks | seconds 29 and 59 | same |
| Minute marker | 800 ms of 1000 Hz | 800 ms of 1200 Hz |
| Hour marker | 800 ms of 1500 Hz | same |
| Tones (seconds 1–45) | 500 Hz even, 600 Hz odd minutes | 600 Hz even, 500 Hz odd |
| 440 Hz | minute 2, except hour 00 | minute 1, except hour 00 |
| Silent minutes | 29, 43–51, 59 | 0, 8–10, 14–19, 30 |
| Station ID | minutes 0 and 30 | minutes 29 and 59 |
| Voice announcement | starts at :52.5 | starts at :45 |
| UT1 correction | doubled ticks 1–8 (+) or 9–16 (−) | same |

The announcement says *"At the tone, H hours, M minutes, Coordinated
Universal Time."*  The minute marker that follows is "the tone".

Relative levels follow the station modulation: ticks and markers
100 %, voice 75 %, tones and time code 50 %.

### 100 Hz time code

A modified IRIG-H code, one bit per second, least significant bit
first.  A pulse of increased 100 Hz amplitude (3.3:1) starts 30 ms
after the second: 170 ms = 0, 470 ms = 1, 770 ms = position marker.
Second 0 carries no pulse; together with P0 at second 59 it marks the
start of the frame.  The frame refers to UTC at its start.

| Seconds | Content |
|---------|---------|
| 2 | DST in effect at 00:00 UTC today |
| 3 | Leap second warning (always 0) |
| 4–7 | Year, units |
| 9, 19, 29, 39, 49, 59 | Position markers P1–P5, P0 |
| 10–13, 15–17 | Minutes, units and tens |
| 20–23, 25–26 | Hours, units and tens |
| 30–33, 35–38, 40–41 | Day of year, units, tens and hundreds |
| 50 | UT1 sign (1 = add to UTC) |
| 51–54 | Year, tens |
| 55 | DST in effect at 24:00 UTC today |
| 56–58 | UT1 correction, 0.1 s units |

### Differences from the radio stations

- **DST rules:** the DST bits follow European rules by default
  (`dst = eu`).  Set `dst = us` for the US rules used by NIST.
- **Leap second warning:** not set.  utbd has no leap second source.
- **Agency announcements:** the minutes NIST uses for agency
  announcements (storm warnings, GPS status, geophysical alerts) carry
  the normal tone.
- **Doubled ticks:** SP 250-67 does not give the spacing of the
  second pulse.  utbd sends it 100 ms after the first.

### Over the telephone

The 100 Hz time code is below the 300–3400 Hz telephone band.  It
passes a G.711 VoIP path unchanged, but an analog line or handset
filters it out.  Ticks, markers, tones and voice are all in band.

---

## Dependencies

Both are standard distribution packages:

| Library | Debian / Ubuntu | Fedora / RHEL |
|---------|-----------------|---------------|
| spandsp | `libspandsp-dev` | `spandsp-devel` |
| Sofia-SIP | `libsofia-sip-ua-dev` | `sofia-sip-devel` |

```bash
apt install build-essential pkg-config libspandsp-dev libsofia-sip-ua-dev
```

The Makefile also finds the Sofia-SIP headers without pkg-config.
`sip.c` / `sip.h` hold the SIP user agent and RTP code, shared with
actsd and actsc.  For the voice files: Piper (`pip install piper-tts`, recommended) or
`espeak-ng`, and optionally `sox` or `ffmpeg` for resampling.

---

## Building and Installing

```bash
make                         # libraries are found with pkg-config
PIPER_MODEL=/path/to/en_US-john-medium.onnx make voice   # see "Voice files"
sudo make install-service
sudo nano /etc/utbd/utbd.conf
sudo systemctl enable --now utbd
journalctl -fu utbd
```

### Voice files

The announcement is assembled from WAV files in `voice_dir`:
`at_the_tone`, `hour`, `hours`, `minute`, `minutes`, `utc`,
`station_id`, `0`–`19`, `20`, `30`, `40`, `50` (all `.wav`, 16-bit PCM,
any sample rate).  Without the files, utbd runs without voice
announcements.

`gen-voice.sh [DIR] [wwv|wwvh]` creates them:

- **Piper** (neural TTS, natural sounding) when `PIPER_MODEL` points to
  a voice model.  Recommended voices, both trained only on
  public-domain LibriVox recordings:
  [en_US-john-medium](https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/john/medium)
  (male) for WWV and
  [en_US-kristin-medium](https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/kristin/medium)
  (female) for WWVH.  See [License](#license) about other voices.  Download the
  `.onnx` and `.onnx.json` files of the voice.
- **espeak-ng** otherwise.  It is available everywhere but sounds
  robotic.

```bash
pip install piper-tts
PIPER_MODEL=en_US-john-medium.onnx ./gen-voice.sh voice wwv
```

`PITCH=-3` makes the voice deeper by that many semitones (sox or
ffmpeg required).  Up to about −3 the words stay as clear as the
original.

Recordings of a human voice saved under the same names sound best.
Any sample rate works: utbd band-limits and resamples the files to
8 kHz, and trims the silence TTS engines add around each word.

---

## Configuration

`/etc/utbd/utbd.conf` (`key = value`, `#` comments).  Command-line
options override the file.

| Key | Default | Meaning |
|-----|---------|---------|
| `registrar` | — | SIP PBX or provider to register with, `host[:port]` |
| `username`, `password` | — | SIP account (username required with a registrar) |
| `allow` | the registrar | Who may call in: `any`, or IPv4 addresses / CIDRs |
| `sip_port` | 5060 | Local SIP port (UDP) |
| `rtp_ports` | 10200-10399 | RTP port range (UDP), two ports per simultaneous call |
| `local_ip` | automatic | Local address for SIP and RTP (default: the one that routes to the registrar) |
| `public_ip` | — | Address advertised in SIP and SDP behind NAT |
| `station` | `wwv` | `wwv` or `wwvh` format |
| `voice_dir` | `/usr/local/share/utbd/voice` | Voice WAV files |
| `dut1` | 0 | UT1 − UTC in 0.1 s (−7..7), from IERS Bulletin A |
| `dst` | `eu` | DST rules for the time code: `eu`, `us`, `none` |
| `advance_ms` | 0 | Send the signal early to offset path delay |
| `max_calls` | 8 | Simultaneous listeners |
| `max_call_sec` | 180 | Call length limit |

Either `registrar` or `allow` must be set.

### Connecting to a PBX

utbd registers like a SIP phone; the PBX routes a number to it.
Asterisk (PJSIP):

```ini
; pjsip.conf — needs a UDP transport, e.g. [transport-udp]
[utbd]
type=endpoint
context=from-internal
disallow=all
allow=alaw,ulaw
auth=utbd-auth
aors=utbd

[utbd-auth]
type=auth
auth_type=userpass
username=utbd
password=changeme

[utbd]
type=aor
max_contacts=1
qualify_frequency=60

; extensions.conf — route a number to utbd
exten => 1236,1,Dial(PJSIP/utbd)
```

Without a PBX, set `allow` to the callers' addresses and call
`sip:time@<utbd address>` directly.

Open `sip_port` and `rtp_ports` (UDP) in the firewall for the PBX.
Behind NAT, forward them and set `public_ip`.

### Upgrading from 1.x

utbd 1.x used IAX2.  Replace `host`/`port` with
`registrar = host[:port]`, `peername` with `username` and `secret`
with `password`, and replace the `iax.conf` peer with a PJSIP
endpoint as above.

---

## Testing Without a PBX

`--render` writes the signal to an 8 kHz WAV file:

```bash
# Two minutes from the current minute
utbd --render wwv.wav --duration 120 --voice-dir voice -c /dev/null

# Around the top of an hour, WWVH format
utbd --render top.wav --start "2026-10-24 22:59" --duration 180 --station wwvh -c /dev/null
```

---

## Security

utbd only reads its own audio files and sends audio.  It does not
interpret anything a caller sends: received RTP is discarded.  It
accepts calls only from `allow` (by default, the registrar's
address); others get `403 Forbidden`.  RTP is sent only to the
address negotiated in SDP.  Caller ID is sanitised before logging, and calls are limited
in number and length.  The systemd unit runs utbd as an unprivileged
user with a system-call filter and a read-only file system.

---

## License

Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

The full license text is in [COPYING](COPYING).
`SPDX-License-Identifier: GPL-3.0-or-later`

### Third-party components

The program is linked with these libraries; no third-party source
code is included in this repository.

| Component | License | Project |
|-----------|---------|---------|
| spandsp | LGPL-2.1-only (library) | https://github.com/freeswitch/spandsp |
| Sofia-SIP | LGPL-2.1-or-later | https://github.com/freeswitch/sofia-sip |

Both libraries may be used by GPL-3.0 programs.  Distribution packages
of Sofia-SIP may link OpenSSL 3 (Apache-2.0), which is compatible with
GPL-3.0 as well.

**Note for binary distributors.**  Besides its LGPL-2.1 code, the
spandsp library contains a few files licensed GPL-2.0-only
(`image_translate.c` in 0.0.6; also `sprt.c` and `v150_1*.c` in newer
snapshots).  GPL-2.0-only and GPL-3.0 code cannot be combined in one
work.  This program does not use those parts, and none of this affects
building it from source or running it.  If you distribute *binaries*,
link spandsp statically (the linker then includes only the LGPL-2.1
modem objects that are used, and the LGPL-2.1 relinking terms apply)
or use a spandsp build without the GPL-2.0-only files.

### Voice files

`gen-voice.sh` runs Piper or espeak-ng as separate programs.  Both are
GPL-3.0-or-later; their license does not cover the speech they
produce.  The voice *models* have their own terms:

| Voice | Use | Training data |
|-------|-----|---------------|
| [en_US-john-medium](https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/john/medium) | WWV (male) | Public-domain LibriVox recordings (fine-tuned from kristin) |
| [en_US-kristin-medium](https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/kristin/medium) | WWVH (female) | Public-domain LibriVox recordings |

Avoid voices fine-tuned from `lessac` (such as `joe`, `mike` and
`ryan`): the lessac dataset is licensed for research use only.  The
public-domain `norman` voice is not recommended either: it garbles
most isolated words, such as the digits of the announcement.

The broadcast format follows NIST Special Publication 250-67, a work
of the U.S. Government.
