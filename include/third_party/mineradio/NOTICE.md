# Mineradio native visual adaptation

Upstream: https://github.com/XxHuberrr/Mineradio-paused
Commit: d43de565acabfdc1a9c9820a27e81a98ccbebcef
License: GPL-3.0; original LICENSE and UPSTREAM-NOTICE.md retained here.

Original sources: public/js/modules/02-visual/00-pointer-cover-particles.js,
01-float-skull-backcover.js, 08-lyrics-display-modes.js, 11-lyrics-shaders.js,
14-stage-lyrics-rendering.js and 01-scene/03-focus-cinema-camera.js.
Original skull point data: public/assets/skull-decimation-points.bin.

Native adaptations change shader language, instancing/sprites, texture upload,
clock/identity handling, audio inputs and camera host. Six lyric styles retain
their source profile values and shader surface formulas. No browser, Three.js,
Electron or remote service runs in HTune.

The two Sonic Topography entries use original HTune native terrain code;
restricted upstream shader code and bundled web application are excluded.
