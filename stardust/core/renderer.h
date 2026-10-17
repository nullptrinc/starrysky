/* SPDX-License-Identifier: MIT */
/* GLES2 drawing for the render core: rounded rectangles and glyph-atlas text. */
#pragma once

#include <GLES2/gl2.h>
#include <map>
#include <string>
#include <tuple>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "tree.h"

namespace stardust {

/* How a rectangle's surface is drawn; sizes are logical pixels. */
struct Material {
	Color top, bottom; // a vertical gradient; a transparent bottom means solid top
	float gloss = 0; // glass: a brighter upper half with a horizon in the middle
	float border = 0;
	Color border_color;
	float bevel = 0; // lit top edge and shaded bottom edge; negative is sunken
	float inner_shadow = 0; // depth of a recess
	float shadow = 0; // blur of the drop shadow below
	Color shadow_color;
	float glow = 0, inner_glow = 0; // light around and inside the edge
	Color glow_color;
	Surface surface = Surface::Plain;

	bool plain() const {
		return gloss == 0 && border == 0 && bevel == 0 && inner_shadow == 0 && shadow == 0 &&
			glow == 0 && inner_glow == 0 && surface == Surface::Plain && bottom.a <= 0;
	}
};

struct TextStyle {
	Color shadow; // drawn text_shadow_offset below the text
	float shadow_offset = 1;
	Color glow_color;
	float glow = 0;
};

class Renderer : public TextMetrics {
public:
	bool init(const char *regular_font, const char *bold_font);
	/* Starts a frame; coordinates passed later are logical pixels. */
	void begin(int width_px, int height_px, float scale);
	/* The direction chrome reflects light from, in radians. */
	void set_light(float angle) { light_ = angle; }
	void rect(float x, float y, float w, float h, float radius, Color color);
	/* alpha is the node's opacity; it fades every part of the material. */
	void rect(float x, float y, float w, float h, float radius, const Material &material,
		float alpha);
	void text(float x, float y, const std::string &text, float size, bool bold, Color color,
		const TextStyle &style = {});

	void lines(float x, float y, float w, float h, const std::vector<float> &points, Color color);

	float text_width(const std::string &text, float size, bool bold) override;
	float line_height(float size) override;

private:
	struct Glyph {
		float u0, v0, u1, v1; // atlas coordinates
		int width, height, left, top;
		float advance;
	};
	/* Shelf-packed alpha textures; a full one starts over. */
	struct Atlas {
		GLuint texture = 0;
		int size = 1024, shelf_x = 0, shelf_y = 0, shelf_h = 0;
		void create();
		/* Uploads a w×h bitmap; false when it no longer fits (call reset first). */
		bool place(int w, int h, const unsigned char *pixels, Glyph &glyph);
		void reset() { shelf_x = shelf_y = shelf_h = 0; }
	};
	const Glyph *glyph(uint32_t codepoint, int pixel_size, bool bold);
	/* The glyph blurred by a Gaussian of the given radius, for backlit text. */
	const Glyph *glow_glyph(uint32_t codepoint, int pixel_size, bool bold, int radius);
	/* Quads for a line of text from one of the glyph sources. */
	template <typename Source>
	std::vector<float> layout_text(float x, float y, const std::string &text, int pixel_size,
		bool bold, Source source);
	void set_size(bool bold, int pixel_size);

	GLuint rect_program_ = 0, text_program_ = 0, material_program_ = 0;
	GLint rect_screen_, rect_half_, rect_radius_, rect_color_;
	GLint text_screen_, text_color_, text_atlas_, text_offset_;
	struct {
		GLint screen, half, radius, top, bottom, border_color, shadow_color, glow_color;
		GLint border, gloss, bevel, inner_shadow, shadow, glow, inner_glow, surface, light,
			scale, alpha;
	} material_;
	float light_ = 3.5f;
	Atlas atlas_, glow_atlas_;
	std::map<std::tuple<uint32_t, int, bool>, Glyph> glyphs_;
	std::map<std::tuple<uint32_t, int, bool, int>, Glyph> glow_glyphs_;
	FT_Library library_ = nullptr;
	FT_Face faces_[2] = {nullptr, nullptr};
	int face_size_[2] = {0, 0};
	float scale_ = 1;
	int width_px_ = 0, height_px_ = 0;
};

} // namespace stardust
