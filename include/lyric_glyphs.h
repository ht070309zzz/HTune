#pragma once
#include <dwrite.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace stage::glyphs {
// One shaped DirectWrite glyph run, copied out of an immutable text layout so
// the stage can animate glyphs without reshaping text. Text positions refer to
// the layout's UTF-16 string; word timing maps onto them unchanged.
struct Run {
    Microsoft::WRL::ComPtr<IDWriteFontFace> face;
    float emSize=0;
    UINT32 bidiLevel=0;
    BOOL sideways=FALSE;
    DWRITE_MEASURING_MODE measuring=DWRITE_MEASURING_MODE_NATURAL;
    D2D1_POINT_2F origin{};               // baseline origin, layout coordinates
    float ascent=0,descent=0;             // DIPs, positive
    std::vector<UINT16> indices;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    std::vector<UINT32> textPositions;    // first UTF-16 index of each glyph's cluster
    std::vector<UINT32> textLengths;      // UTF-16 length of that cluster (0 for trailing glyphs)
    std::vector<FLOAT> pen;               // glyph origin x relative to origin.x (LTR order)
    bool Rtl() const {return (bidiLevel&1)!=0;}
};
struct Layout {
    std::vector<Run> runs;
    float width=0,height=0;
    bool valid=false;
};
// Captures every glyph run of layout. Returns S_OK with valid=false when the
// layout uses features the stage does not animate (for example inline objects);
// callers then keep drawing the layout as a whole.
HRESULT Capture(IDWriteTextLayout* layout,Layout& out);
// Builds a glyph run for the half-open glyph range [first,last) of run.
DWRITE_GLYPH_RUN Slice(const Run& run,size_t first,size_t last);
}
