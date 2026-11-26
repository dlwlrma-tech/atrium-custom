// The end cards of a strip that goes on (#112): the last one's picture sharp at its top, blurred more
// and more down it, and gone at its bottom; the first one the same way up. Made once a picture, in the picture's own px, so the perspective
// that turns the card turns the fade with it.
#pragma once

#include "icon_image.h"

namespace atrium {

struct StageFade {
    int width = 0;      // the picture's px across the faded image (`margin` and all): it may be made smaller
    float margin = 0;   // the transparent px around it, in the source's px, for the blur to spread into
    float maxBlur = 0;  // the blur's standard deviation at the bottom, in the source's px
};

// How the picture `sourceWidth` px wide is faded for a card `cardPx` px wide on the screen.
StageFade StageFadeFor(int sourceWidth, float cardPx);

// `image` faded away down (`up`: up): rows from `top` to `bottom` (source px) go from sharp to
// `fade.maxBlur`, and their opacity from full to none over the far part. Premultiplied BGRA in and
// out, `fade.width` px wide plus `fade.margin` scaled around.
IconImage FadeAway(const IconImage& image, float top, float bottom, const StageFade& fade, bool up);

// How much of a row `v` of the way along the fade (0 sharp, 1 its far end) is kept, and how blurred it is (0..1).
float StageFadeOpacity(float v);
float StageFadeBlur(float v);

} // namespace atrium
