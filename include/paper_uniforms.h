// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <string_view>
namespace stage {
struct PaperUniformMap {std::string_view name;std::array<std::string_view,64> fields;};
inline constexpr std::array<PaperUniformMap,7> PaperUniforms={{
{"dithering",{"u_time","u_resolution","u_pixelRatio","u_originX","u_originY","u_worldWidth","u_worldHeight","u_fit","u_scale","u_rotation","u_offsetX","u_offsetY","u_pxSize","u_colorBack","u_colorFront","u_shape","u_type"}},
{"mesh-gradient",{"u_time","u_colorsCount","u_distortion","u_swirl","u_grainMixer","u_grainOverlay","u_resolution"}},
{"image-dithering",{"u_resolution","u_pixelRatio","u_originX","u_originY","u_fit","u_scale","u_rotation","u_offsetX","u_offsetY","u_colorFront","u_colorBack","u_colorHighlight","u_imageAspectRatio","u_type","u_pxSize","u_originalColors","u_inverted","u_colorSteps"}},
{"halftone-dots",{"u_colorFront","u_colorBack","u_radius","u_contrast","u_imageAspectRatio","u_size","u_grainMixer","u_grainOverlay","u_grainSize","u_grid","u_originalColors","u_inverted","u_type","u_resolution"}},
{"fluted-glass",{"u_resolution","u_pixelRatio","u_colorBack","u_colorShadow","u_colorHighlight","u_imageAspectRatio","u_size","u_shadows","u_angle","u_stretch","u_shape","u_distortion","u_highlights","u_distortionShape","u_shift","u_blur","u_edges","u_marginLeft","u_marginRight","u_marginTop","u_marginBottom","u_grainMixer","u_grainOverlay"}},
{"paper-texture",{"u_isImage","u_imageAspectRatio","u_colorBack","u_colorPaper","u_colorShadow","u_blending","u_distortion","u_clip","u_angle","u_seed","u_roughness","u_roughnessSize","u_roughnessRows","u_fiber","u_fiberSize","u_folds","u_foldSizeX","u_foldSizeY","u_foldOffsetX","u_foldOffsetY","u_wrinkles","u_wrinkleSize","u_crumples","u_crumpleCount","u_drops","u_resolution"}},
{"lens-distortion",{"u_imageAspectRatio","u_spread","u_bias","u_angle","u_perspective","u_count","u_dispersion","u_dispersionShift","u_dispersionColor","u_focusCenter","u_focusEdges","u_swirl","u_noise","u_noiseFrequency","u_noiseOffset","u_lensBulge","u_lensCircle","u_grainMixer","u_grainOverlay","u_imageX","u_imageY","u_resolution"}},
}};
}
