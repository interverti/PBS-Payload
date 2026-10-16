# Building PBS Payload

## PS5 payload (Windows)

Install LLVM with clang/lld and a compatible PS5 Payload SDK. Supply the SDK
explicitly; developer-specific absolute paths are not used.

```powershell
./payload/build.ps1 -SdkPath 'C:/path/to/ps5-payload-sdk'
```

Output: `payload/build/pbs.elf`. Browser assets are embedded during the build.
No arguments starts streaming with audio, 1440p, 60 fps mode, port 8080 and a
60 Mbit/s requested encoder budget. Optional loader arguments:

```text
avserve [720p|1080p|1440p] [bitrate=1000000..100000000] [minqp=18..35] [noaudio] [port]
```

If a higher bitrate start is rejected, capture retries the conservative
30 Mbit/s configuration. Runtime capability depends on console output and
capture APIs. The cleaned build has not been redeployed to a console yet.

## Transport regression test

The RTP packetizer test requires a C++20 compiler and no console or client.

```sh
g++ -std=c++20 -Ipayload/src tests/rtp_packetizer.cpp -o rtp_packetizer
./rtp_packetizer
```

It checks single-NAL and FU-A packets, MTU, timestamps, marker bits and
sequence-number wrap. Put generated executables in an ignored build directory.
