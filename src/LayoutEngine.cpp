/*
 * (C) Copyright IBM Corp. 1998-2011 - All Rights Reserved
 * (C) Copyright Google, Inc. 2012 - All Rights Reserved
 *
 * Google Author(s): Behdad Esfahbod
 */

#include "LETypes.h"
#include "LEScripts.h"
#include "LELanguages.h"
#include "LEFontInstance.h"

#include "LayoutEngine.h"

#include <hb.h>
#include <hb-icu.h>

U_NAMESPACE_BEGIN

/* Leave this copyright notice here! It needs to go somewhere in this library. */
static const char copyright[] = U_COPYRIGHT_STRING;

const le_int32 LayoutEngine::kTypoFlagKern = 0x1;
const le_int32 LayoutEngine::kTypoFlagLiga = 0x2;

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(LayoutEngine)


static hb_blob_t *
icu_le_hb_reference_table (hb_face_t *face, hb_tag_t tag, void *user_data)
{
  const LEFontInstance *fontInstance = (const LEFontInstance *) user_data;

  size_t length = 0;
  const char *data = (const char *) fontInstance->getFontTable (tag, length);

  return hb_blob_create (data, length, HB_MEMORY_MODE_READONLY, NULL, NULL);
}

static hb_bool_t
icu_le_hb_font_get_glyph (hb_font_t *font,
			  void *font_data,
			  hb_codepoint_t unicode,
			  hb_codepoint_t variation_selector,
			  hb_codepoint_t *glyph,
			  void *user_data)

{
  const LEFontInstance *fontInstance = (const LEFontInstance *) font_data;

  *glyph = fontInstance->mapCharToGlyph (unicode);
  return !!glyph;
}

static hb_position_t
icu_le_hb_font_get_glyph_h_advance (hb_font_t *font,
				    void *font_data,
				    hb_codepoint_t glyph,
				    void *user_data)
{
  const LEFontInstance *fontInstance = (const LEFontInstance *) font_data;
  LEPoint advance;

  fontInstance->getGlyphAdvance (glyph, advance);

  return advance.fX;
}

static hb_bool_t
icu_le_hb_font_get_glyph_contour_point (hb_font_t *font,
				        void *font_data,
				        hb_codepoint_t glyph,
				        unsigned int point_index,
				        hb_position_t *x,
				        hb_position_t *y,
				        void *user_data)
{
  const LEFontInstance *fontInstance = (const LEFontInstance *) font_data;
  LEPoint point;

  if (!fontInstance->getGlyphPoint (glyph, point_index, point))
    return false;

  *x = point.fX;
  *y = point.fY;

  return true;
}

static hb_font_funcs_t *
icu_le_hb_get_font_funcs (void)
{
  static hb_font_funcs_t *ffuncs = NULL;

retry:
  if (!ffuncs) {
    /* Only pseudo-thread-safe... */
    hb_font_funcs_t *f = hb_font_funcs_create ();
    hb_font_funcs_set_glyph_func (f, icu_le_hb_font_get_glyph, NULL, NULL);
    hb_font_funcs_set_glyph_h_advance_func (f, icu_le_hb_font_get_glyph_h_advance, NULL, NULL);
    hb_font_funcs_set_glyph_contour_point_func (f, icu_le_hb_font_get_glyph_contour_point, NULL, NULL);

    if (!ffuncs)
      ffuncs = f;
    else {
      hb_font_funcs_destroy (f);
      goto retry;
    }
  }

  return ffuncs;
}

LayoutEngine::LayoutEngine(const LEFontInstance *fontInstance,
                           le_int32 scriptCode,
                           le_int32 languageCode,
                           le_int32 typoFlags,
                           LEErrorCode &success)
  : fHbFont(NULL), fHbBuffer(NULL), fTypoFlags(typoFlags)
{
    if (LE_FAILURE(success)) {
        return;
    }

    fHbBuffer = hb_buffer_create ();
    if (fHbBuffer == hb_buffer_get_empty ()) {
	success = LE_MEMORY_ALLOCATION_ERROR;
	return;
    }
    hb_buffer_set_unicode_funcs (fHbBuffer, hb_icu_get_unicode_funcs ());
    hb_buffer_set_script (fHbBuffer, hb_icu_script_to_script ((UScriptCode) scriptCode));
    /* TODO set language */

    hb_face_t *face = hb_face_create_for_tables (icu_le_hb_reference_table, (void *) fontInstance, NULL);
    fHbFont = hb_font_create (face);
    hb_face_destroy (face);
    if (fHbFont == hb_font_get_empty ()) {
        success = LE_MEMORY_ALLOCATION_ERROR;
	return;
    }
    hb_font_set_funcs (fHbFont, icu_le_hb_get_font_funcs (), (void *) fontInstance, NULL);
    hb_font_set_scale (fHbFont,
		       +fontInstance->getXPixelsPerEm () * fontInstance->getScaleFactorX (),
		       -fontInstance->getYPixelsPerEm () * fontInstance->getScaleFactorY ());
    hb_font_set_ppem (fHbFont,
		      fontInstance->getXPixelsPerEm (),
		      fontInstance->getYPixelsPerEm ());
}

LayoutEngine::~LayoutEngine(void)
{
    hb_font_destroy (fHbFont);
    hb_buffer_destroy (fHbBuffer);
}

le_int32 LayoutEngine::getGlyphCount() const
{
    return hb_buffer_get_length (fHbBuffer);
}

void LayoutEngine::getCharIndices(le_int32 charIndices[], le_int32 indexBase, LEErrorCode &success) const
{
  if (LE_FAILURE (success)) return;
  unsigned int count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos (fHbBuffer, &count);
  for (unsigned int i = 0; i < count; i++)
    charIndices[i] = info[i].cluster + indexBase;
}

void LayoutEngine::getCharIndices(le_int32 charIndices[], LEErrorCode &success) const
{
  if (LE_FAILURE (success)) return;
  unsigned int count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos (fHbBuffer, &count);
  for (unsigned int i = 0; i < count; i++)
    charIndices[i] = info[i].cluster;
}

// Copy the glyphs into caller's (32-bit) glyph array, OR in extraBits
void LayoutEngine::getGlyphs(le_uint32 glyphs[], le_uint32 extraBits, LEErrorCode &success) const
{
  if (LE_FAILURE (success)) return;
  unsigned int count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos (fHbBuffer, &count);
  for (unsigned int i = 0; i < count; i++)
    glyphs[i] = info[i].codepoint | extraBits;
}

void LayoutEngine::getGlyphs(LEGlyphID glyphs[], LEErrorCode &success) const
{
  if (LE_FAILURE (success)) return;
  unsigned int count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos (fHbBuffer, &count);
  for (unsigned int i = 0; i < count; i++)
    glyphs[i] = info[i].codepoint;
}


void LayoutEngine::getGlyphPositions(float positions[], LEErrorCode &success) const
{
  if (LE_FAILURE (success)) return;
  unsigned int count;
  const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (fHbBuffer, &count);
  float x = 0, y = 0;
  unsigned int i;
  for (i = 0; i < count; i++) {
    positions[2 * i]     = x + pos[i].x_offset;
    positions[2 * i + 1] = y + pos[i].y_offset;
    x += pos[i].x_advance;
    y += pos[i].y_advance;
  }
  positions[2 * i]     = x;
  positions[2 * i + 1] = y;
}

void LayoutEngine::getGlyphPosition(le_int32 glyphIndex, float &x, float &y, LEErrorCode &success) const
{
  if (LE_FAILURE (success)) return;
  unsigned int count;
  const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (fHbBuffer, &count);
  unsigned int i;
  x = 0; y = 0;
  for (i = 0; i < (unsigned int) glyphIndex; i++) {
    x += pos[i].x_advance;
    y += pos[i].y_advance;
  }
  x += pos[glyphIndex].x_offset;
  y += pos[glyphIndex].y_offset;
}

// Input: characters, font?
// Output: glyphs, positions, char indices
// Returns: number of glyphs
le_int32 LayoutEngine::layoutChars(const LEUnicode chars[], le_int32 offset, le_int32 count, le_int32 max, le_bool rightToLeft,
				   float x, float y, LEErrorCode &success)
{
    if (LE_FAILURE(success)) {
        return 0;
    }

    if (chars == NULL || offset < 0 || count < 0 || max < 0 || offset >= max || offset + count > max) {
        success = LE_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    hb_buffer_set_length (fHbBuffer, 0);
    hb_buffer_set_direction (fHbBuffer, rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_add_utf16 (fHbBuffer, chars, max, offset, count);

    hb_shape (fHbFont, fHbBuffer, NULL, 0);

    return hb_buffer_get_length (fHbBuffer);
}

void LayoutEngine::reset()
{
    hb_buffer_set_length (fHbBuffer, 0);
}

LayoutEngine *LayoutEngine::layoutEngineFactory(const LEFontInstance *fontInstance, le_int32 scriptCode, le_int32 languageCode, LEErrorCode &success)
{
  // 3 -> kerning and ligatures
  return LayoutEngine::layoutEngineFactory(fontInstance, scriptCode, languageCode, 3, success);
}

LayoutEngine *LayoutEngine::layoutEngineFactory(const LEFontInstance *fontInstance, le_int32 scriptCode, le_int32 languageCode, le_int32 typoFlags, LEErrorCode &success)
{
    if (LE_FAILURE(success)) {
        return NULL;
    }

    LayoutEngine *result = NULL;
    result = new LayoutEngine(fontInstance, scriptCode, languageCode, typoFlags, success);

    if (result && LE_FAILURE(success)) {
		delete result;
		result = NULL;
	}

    if (result == NULL) {
        success = LE_MEMORY_ALLOCATION_ERROR;
    }

    return result;
}

U_NAMESPACE_END
