# Third-party notices

## QQMusicDecoder (QRC format decoder)

`include/third_party/qqmusicdecoder/qrc_des.h` is a C++ adaptation of
[WXRIW/QQMusicDecoder DESHelper.cs](https://github.com/WXRIW/QQMusicDecoder/blob/master/QQMusicDecoder/DESHelper.cs).
Copyright (c) 2023 WXRIW, MIT. The full license is retained in
`include/third_party/qqmusicdecoder/LICENSE`. Adaptation preserves the QRC-specific
S-boxes and byte order, replaces managed arrays with bounded native arrays and
uses unsigned arithmetic for S-box shifts. No C# or .NET runtime is used.

## miniz 3.0.2 (inflate only)

The native QRC decoder uses the inflate implementation from
[richgel999/miniz 3.0.2](https://github.com/richgel999/miniz/tree/3.0.2), MIT.
Full license and copyright notices are retained in `include/third_party/miniz`.
Only `miniz_tinfl.c`, `miniz_tinfl.h`, `miniz_common.h` are included; the source's
umbrella include is replaced by `miniz_tinfl.h`, with a local static export header.
HTune supplies a fixed 2 MiB output buffer and requires zlib checksum validation.

HTune contains or adapts the following open-source work. Folia (https://github.com/chthollyphile/folia-major) and Mineradio (https://github.com/XxHuberrr/Mineradio-paused) were visual and interaction references only; their Electron runtimes, names, icons and bundled assets are not included in this application.

Compatibility research also used binary-layout observations published by `hanbinhsh/NetEase_Cloud_Music_Local_API` at commit `4efc61a733ad9939f21bd5c7211abb4b78078fe3`. That repository did not expose a redistribution license when reviewed, so none of its Python source or assets are included; `src/client_clock.cpp` is an independent native implementation with additional read-only and validation constraints.

## nlohmann/json 3.12.0

Project: <https://github.com/nlohmann/json>

Copyright (c) 2013-2025 Niels Lohmann

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Widdit/now-playing-service

Adapted component: `NeteaseMusicHelper.cs` from commit `2336c5fc6c57300bc7cf5fa85c7f273042bb86b1`, rewritten for native C++/COM in `src/playback.cpp`.

Project: <https://github.com/Widdit/now-playing-service>

Copyright (c) 2024 Widdit

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## QQ current song identity layout

`src/qq_identity.cpp` uses the public QQ current-song structure and instruction
pattern documented by [Kxnrl/NetEase-Cloud-Music-DiscordRPC](https://github.com/Kxnrl/NetEase-Cloud-Music-DiscordRPC/blob/3a97a4b5c8b673ce2f69b0205adbc6a3ac861ba5/Vanessa/Players/Tencent.cs).
Copyright (c) 2018 Kyle, MIT; full license is retained in
`include/third_party/qqidentity/LICENSE`. HTune supplies its own C++ reader,
exact binary fingerprint, bounded reads, stable-snapshot checks and SMTC
corroboration. No plugin, hook, process injection or client modification is used.

## Kugou playback layout research

The position offset in `src/kugou_clock.cpp` follows
[AF-Media-Bar KuGou source](https://github.com/Fervent-Tempo/AF-Media-Bar/blob/78faedb341fcf2cbd2c06fe2e1ec1adef0183b49/src/AFMediaBar/Classes/Services/Media/Sources/KuGou/KuGou.cs),
MIT, Copyright (c) 2026 AmorFate. Full license: `include/third_party/kugou-layout/LICENSE`.
HTune implements its own bounded C++ reader with exact two-DLL fingerprints,
process lifetime validation and independent duration layout research. No C# runtime
or injection is used. KRC is independently parsed in C++; its zlib stream uses
the same bounded miniz implementation and license listed above.
