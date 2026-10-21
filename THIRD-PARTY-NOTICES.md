# Third-party notices

## Optional lyric fonts added in 0.7.28

Three unmodified, privately loaded font choices are bundled for optional DIY
selection. These fonts are not installed into Windows. Their complete SIL Open
Font License 1.1 texts are kept beside each font and embedded in the executable.

- LXGW WenKai Lite Medium: https://github.com/lxgw/LxgwWenKai-Lite at
  `251ab63642d1c1c5db80497d78911b7aae6917d7`; copyright LXGW and the Klee Project
  Authors. License: `assets/fonts/LXGWWenKaiLite-OFL.txt`.
- ZCOOL KuaiLe Regular: Google Fonts repository at
  `47d54ca1ebb14fd864695f1d39dc734f39017c33`, `ofl/zcoolkuaile`; copyright the
  ZCOOL KuaiLe Project Authors. License: `assets/fonts/ZCOOLKuaiLe-OFL.txt`.
- ZCOOL QingKe HuangYou Regular: same fixed Google Fonts commit,
  `ofl/zcoolqingkehuangyou`; copyright the ZCOOL QingKe HuangYou Project Authors.
  License: `assets/fonts/ZCOOLQingKeHuangYou-OFL.txt`.

Exact source URLs, byte counts and SHA-256 are in
`assets/fonts/optional-fonts-sources.json`. User-selected system fonts and local
TTF/OTF files are references only; HTune does not redistribute those files.

HTune original source is licensed under GNU AGPL-3.0-only; see root `LICENSE`.
Third-party materials below retain their respective copyright and license notices.

## Folia native visual adaptation

Upstream: https://github.com/chthollyphile/folia-major, commit
`26c57f9b1b26b402a09e8943171a332215418ce6`, GNU AGPL-3.0.
Original license and adaptation notes: `include/third_party/folia/`.
Native ports include Fume camera math, Tempera vector recipes and timing,
Lumiere light-field shader/100 rigs, Sonnet vector decorations/100 variants,
Diorama path/13 camera curves and formations, full-scene transitions,
Pendolo gear geometry/motion, Common/Sora background motion, and Monet
wash/drift math. Other lyric layouts are adapted to DirectWrite measurements.
Fonts, controls, time/identity gates, GPU hosting and settings remain native.
The exported recipe data in `assets/motion/folia-*.json` is derived work.

## Mineradio native visual adaptation

Upstream: https://github.com/XxHuberrr/Mineradio-paused, commit
`d43de565acabfdc1a9c9820a27e81a98ccbebcef`, GNU GPL-3.0.
Full LICENSE and original NOTICE: `include/third_party/mineradio/`.
`src/shaders/mineradio.hlsl` ports the cover/particle and skull shaders;
`mineradio_lyrics.hlsli` ports the lyric surface shader and six motion profiles.
The removed skull theme's point-model asset is not distributed in this version.
`src/mineradio_stage.cpp` provides native textures, instancing, camera easing
and pause/seek behavior. No Three.js, Electron, Node service or browser ships.

The two Sonic Topography entries are independently reimplemented in
`src/shaders/native_terrain.hlsl`. Sonic Topography's original non-commercial
source is excluded from HTune source inputs, builds and packages. Read-only
study references remain in local evidence only. The original embedded web
application and external Wallpaper Engine projects are not redistributed.

## Paper Shaders

Project: https://github.com/paper-design/shaders, commit
`43cd68db79fa0b1759f72ffc941b3238e2a3954c`, Apache-2.0.
Copyright Lost Coast Labs, Inc. (https://paper.design).
Full LICENSE and NOTICE: `include/third_party/paper-shaders/`.
`src/shaders/paper_0.hlsl` through `paper_6.hlsl` adapt Dithering,
Mesh Gradient, Image Dithering, Halftone Dots, Fluted Glass, Paper Texture,
and Lens Distortion to native HLSL. Adaptations change I/O, uniform packing,
GLSL matrix orientation, constructor splats, texture sampling, scope handling,
and deterministic initialization. Native atlas/viewport mapping, finite power
inputs and bounded anti-aliasing replace platform-specific GL behavior.

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
MusicStage supplies a fixed 2 MiB output buffer and requires zlib checksum validation.

HTune contains or adapts the open-source work listed here. Folia and Mineradio names in these notices identify the credited visual adaptations; their application runtimes are not included.

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
`include/third_party/qqidentity/LICENSE`. MusicStage supplies its own C++ reader,
exact binary fingerprint, bounded reads, stable-snapshot checks and SMTC
corroboration. No plugin, hook, process injection or client modification is used.

## Kugou playback layout research

The position offset in `src/kugou_clock.cpp` follows
[AF-Media-Bar KuGou source](https://github.com/Fervent-Tempo/AF-Media-Bar/blob/78faedb341fcf2cbd2c06fe2e1ec1adef0183b49/src/AFMediaBar/Classes/Services/Media/Sources/KuGou/KuGou.cs),
MIT, Copyright (c) 2026 AmorFate. Full license: `include/third_party/kugou-layout/LICENSE`.
MusicStage implements its own bounded C++ reader with exact two-DLL fingerprints,
process lifetime validation and independent duration layout research. No C# runtime
or injection is used. KRC is independently parsed in C++; its zlib stream uses
the same bounded miniz implementation and license listed above.

## Smiley Sans / 得意黑 v2.0.1

The Rift Live / 裂光现场 theme embeds the unmodified Smiley Sans Oblique
font from https://github.com/atelier-anchor/smiley-sans/releases/tag/v2.0.1.
The original SIL Open Font License 1.1 and copyright notice are retained in
`assets/fonts/SmileySans-OFL.txt` and embedded as resource ID 202. The font is
loaded privately through DirectWrite; it is not installed as a system font.
Font SHA256: `b447d7e781f08bc95c4c9f23ba71ed2b8ebb639aa7184485c71c4ca5afcd25c4`.

## Earlier HTune source (through 0.7.14)

The following original notice is retained for the earlier MIT-licensed HTune code.
Later combined versions use the root AGPL license; historical grants remain intact.

MIT License

Copyright (c) 2026 ht070309zzz and HTune contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
