P2P Messenger - ready-to-use test kit
====================================

This folder is generated next to p2pnode.exe.  All audio files are
quiet sine tones; there are no screamers, random sounds or copyrighted
recordings.

Files:
  tone_440_10s.wav  - 10 s, 440 Hz, low volume
  chirp_5s.wav      - 5 s, 523 Hz, low volume
  file_1mb.bin      - deterministic 1 MiB file
  file_10mb.bin     - deterministic 10 MiB file
  manifest.sha256   - SHA-256 of the generated files
  selftest.ps1      - run p2pnode.exe --selftest and save output

Quick checks:
  1. Start p2pnode-gui.exe on both computers.
  2. Add the other node by Invite URI.
  3. Open the chat and use Attach -> file_1mb.bin.
  4. The receiver shows the completed file; the sender shows
     'all bytes received' only after the receiver's SHA-256 check.
  5. For microphone testing, use the microphone test in the app or run:
     p2pnode.exe --record-mic mic_test.wav --seconds 10
     The command also writes mic_test.wav.json with RMS, peak and clipping.
  6. After a chat/file/voice test open Inspector -> Audio and press
     'Save test report'; send testkit/test_report.txt with the WAV/JSON.
  7. For an automated self-test run testkit/selftest.ps1.

If a direct UDP path is blocked, both nodes should connect through the
same relay/bootstrap node; the handshake and chat then use RELAY
envelopes while the payload stays end-to-end encrypted.
