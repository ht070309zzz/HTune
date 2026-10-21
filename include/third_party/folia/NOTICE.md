# Folia native visual adaptation

Upstream: https://github.com/chthollyphile/folia-major
Pinned commit: `26c57f9b1b26b402a09e8943171a332215418ce6`.
Copyright: Folia contributors. License: GNU AGPL version 3; full upstream LICENSE retained beside this notice.

`src/folia_camera.cpp` adapts the retarget bridge, focus smoothing, positional/scale spring, velocity limits and float from `src/components/visualizer/fume/fumeCameraStep.ts`, plus easing/mix math from `fumeMath.ts`. `include/folia_camera.h` supplies native state/input types. Adaptation date: 2026-10-20.

HTune changes: native renderer supplies measured world-space glyph anchors; frame integration uses bounded substeps; pause freezes motion; seek/identity changes reset directly; playback time replaces wall time for idle float. React, Canvas, Electron, Node and Folia assets are not included in the runtime. No claim of pixel-identical reproduction is made.

The upstream repository, pinned commit and original source paths above identify the adapted source. HTune's original source is offered under AGPL-3.0-only; existing third-party components retain their own licenses and notices. Private development evidence is not part of the public source tree.


Additional native adaptations (2026-10-20): `folia_programs.cpp`,
`folia_diorama.cpp`, `folia_backdrop.cpp`, Folia paths in `rift_stage.cpp`,
`folia_light.hlsl`, `folia_transition.hlsl`, `folia_monet.hlsl` and all
`assets/motion/folia-*.json`. These use the original composition/rig recipes,
light-field fragment shader, path/shot/formation math, paragraph transitions,
gears, background drift and color-wash functions at the pinned revision.
Native changes use DirectWrite clusters, D2D geometry caches, transparent
D3D11 layers, playback time, bounded integration and current audio envelopes.
No exact beat map is inferred from spectral envelopes. See root notices for
Paper shaders and other separately licensed dependencies.
