P2P Messenger - Windows portable build
=====================================

Build machine (MSYS2 MINGW64 shell)
-----------------------------------
Install the toolchain and the three native libraries once:

  pacman -S --needed mingw-w64-x86_64-gcc \
      mingw-w64-x86_64-SDL2 \
      mingw-w64-x86_64-opus \
      mingw-w64-x86_64-libsodium

Then, from this app directory:

  ./build_win.sh

The script produces:

  build/p2pnode.exe       console build (headless tests and CLI)
  build/p2pnode-gui.exe   windowed GUI build
  dist/                   portable folder with the required DLLs

Copy the whole dist/ folder to a friend's PC.  On first launch the app
creates dist/testkit/ next to the executable; it contains:

  tone_440_10s.wav        10 s quiet 440 Hz sine
  chirp_5s.wav            5 s quiet 523 Hz sine
  file_1mb.bin            deterministic 1 MiB test file
  file_10mb.bin           deterministic 10 MiB test file
  manifest.sha256         SHA-256 of all generated files
  mic_test.ps1            capture-only microphone check
  selftest.ps1            runs p2pnode.exe --selftest into selftest_report.txt
  README.txt              short instructions for the test person

No screamer, alarm, random sound or copyrighted recording is included.
The test tone is intentionally quiet.  The microphone check only records;
it does not play anything through the speakers.

Quick test
----------
1. Start p2pnode-gui.exe on both computers.
2. Click "Copy invite" on one side and add the URI on the other.
3. If the direct UDP path is blocked, select the relay/bootstrap node in the
   Network inspector and use "Connect via selected relay" in Add contact.
4. Open the chat, click Attach, and send testkit/file_1mb.bin.  The
   current sender uses stop-and-wait, so allow up to a minute over a relay.
5. The sender displays "all bytes received" only after the receiver has
   checked the SHA-256 and acknowledged every byte.
6. Open Inspector -> Audio and press "Save test report".  Send the generated
   dist/testkit/test_report.txt together with any WAV/JSON microphone files.

Microphone check
----------------
Open PowerShell in the dist/testkit folder:

  powershell -ExecutionPolicy Bypass -File .\mic_test.ps1

This writes mic_test.wav and mic_test.wav.json next to the script.  It opens
only the capture device; no playback device is opened.  The recording is
deliberately not uploaded anywhere.  The JSON records RMS in dBFS, peak and
the number of clipped samples, so the result can be logged without listening
to the file.

Sending data back
-----------------
The GUI writes dist/testkit/test_report.txt.  It contains the node ID, peer
list, relay/direct path, NAT type, group list, file-transfer result, counters
and microphone metrics.  It does not contain message text, session keys or
key seeds.  For an automated run use:

  powershell -ExecutionPolicy Bypass -File .\selftest.ps1

This saves dist/testkit/selftest_report.txt.

Headless smoke test
-------------------
On a development machine with both binaries:

  python tools/test_local.py

It starts a local relay and two console nodes with hidden output, then checks
relay-first HELLO, one chat message, and a 1 MiB file with SHA-256.

Useful flags
------------
  --selftest              security/network/file/relay suite
  --headless              console node; no SDL window
  --record-mic FILE       capture-only microphone WAV
  --relay-id HEX          select relay by NodeID
  --relay-only            never try a direct peer path
  --send-file PATH        send a file and wait for the receiver's ACK
  --report FILE           write a JSON test report
  --report-txt FILE       write a human-readable test report
  --pos X,Y               place the window, including off-screen coordinates
  --size W,H              window size

The relay service is a separate binary (p2p-relay) and is deployed on the
VPS after the runtime checks pass.
