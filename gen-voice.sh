#!/bin/sh
# gen-voice.sh — create the utbd voice files
#
#   ./gen-voice.sh [OUTPUT_DIR] [wwv|wwvh]
#
# PITCH=-3 lowers (or raises) the voice by that many semitones, for a
# deeper voice; this needs sox or ffmpeg.
#
# Speech engine:
#   - Piper neural TTS when PIPER_MODEL points to a voice model (.onnx).
#     Recommended: en_US-john-medium (male, WWV) and
#     en_US-kristin-medium (female, WWVH).  Both were trained only on
#     public-domain LibriVox recordings.  Avoid voices derived from
#     "lessac" (e.g. joe, mike, ryan): its training data is licensed
#     for research only.  See README.md.
#   - espeak-ng otherwise.  It works everywhere but sounds robotic.
#
# Human recordings with the same file names (16-bit PCM WAV, any rate)
# sound best of all.  If sox or ffmpeg is installed the files are
# converted to 8 kHz here with a high-quality resampler; otherwise utbd
# resamples them when it loads them.
#
# Piper and espeak-ng are run as separate programs; their licences
# (both GPL-3.0-or-later) do not extend to the speech they produce.
#
# Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

OUT=${1:-voice}
STATION=${2:-wwv}

case "$STATION" in
    wwv)  ESPEAK_VOICE=${ESPEAK_VOICE:-en-us} ;;
    wwvh) ESPEAK_VOICE=${ESPEAK_VOICE:-en-us+f2} ;;
    *)    echo "usage: $0 [OUTPUT_DIR] [wwv|wwvh]" >&2; exit 1 ;;
esac

if [ -n "${PIPER_MODEL:-}" ]; then
    command -v piper >/dev/null || { echo "piper is not installed (pip install piper-tts)" >&2; exit 1; }
    ENGINE="piper ($(basename "$PIPER_MODEL" .onnx))"
else
    command -v espeak-ng >/dev/null || { echo "espeak-ng is required (or set PIPER_MODEL)" >&2; exit 1; }
    ENGINE="espeak-ng ($ESPEAK_VOICE)"
fi

PITCH=${PITCH:-0}
if [ "$PITCH" != 0 ]; then
    command -v sox >/dev/null || command -v ffmpeg >/dev/null ||
        { echo "PITCH needs sox or ffmpeg" >&2; exit 1; }
    ENGINE="$ENGINE, pitch $PITCH semitones"
fi

mkdir -p "$OUT"
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

# Single words end with a comma and Piper runs with less sampling noise:
# neural voices tend to mumble or add stray sounds to bare words.
say() {     # say FILE TEXT
    if [ -n "${PIPER_MODEL:-}" ]; then
        echo "$2" | piper -m "$PIPER_MODEL" --noise-scale 0.333 \
            --noise-w-scale 0.333 -f "$TMP" 2>/dev/null
    else
        espeak-ng -v "$ESPEAK_VOICE" -s 150 -w "$TMP" "$2"
    fi

    if command -v sox >/dev/null; then
        sox "$TMP" -r 8000 -c 1 -b 16 "$OUT/$1.wav" \
            pitch "$(awk -v p="$PITCH" 'BEGIN { print p * 100 }')" rate -v
    elif command -v ffmpeg >/dev/null; then
        # Pitch shift: play at a changed rate, then restore the tempo
        sr=$(ffprobe -v error -show_entries stream=sample_rate -of csv=p=0 "$TMP")
        set -- "$1" $(awk -v p="$PITCH" -v sr="$sr" \
            'BEGIN { k = 2 ^ (p / 12); printf "%d %.6f", sr * k + 0.5, 1 / k }')
        ffmpeg -loglevel error -y -i "$TMP" \
            -af "asetrate=$2,aresample=$sr,atempo=$3,aresample=filter_size=64:phase_shift=10:cutoff=0.9" \
            -ar 8000 -ac 1 -c:a pcm_s16le "$OUT/$1.wav"
    else
        cp "$TMP" "$OUT/$1.wav"
    fi
}

say at_the_tone "At the tone,"
say hour        "hour,"
say hours       "hours,"
say minute      "minute,"
say minutes     "minutes,"
say utc         "Coordinated Universal Time."
say station_id  "This is the U T C time broadcast service, time dot t gabor dot e u, providing time of day by telephone in the format of the N I S T radio station W W V. This service is not operated by, or affiliated with, N I S T."

n=0
for w in zero one two three four five six seven eight nine ten eleven twelve \
         thirteen fourteen fifteen sixteen seventeen eighteen nineteen; do
    say "$n" "$w,"
    n=$((n + 1))
done
say 20 twenty,
say 30 thirty,
say 40 forty,
say 50 fifty,

echo "Voice files written to $OUT using $ENGINE" >&2
