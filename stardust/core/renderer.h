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

class Renderer : public TextMetrics {
public:
	bool init(const char *regular_font, const char *bold_font);
	/* Starts a frame; coordinates passed later are logical pixels. */
	void begin(int width_px, int height_px, float scale);
	void rect(float x, float y, float w, float h, float radius, Color color);
	void text(float x, float y, const std::string &text, float size, bool bold, Color color);

	void lines(float x, float y, float w, float h, const std::vector<float> &points, Color color);

	float text_width(const std::string &text, float size, bool bold) override;
	float line_height(float size) override;

private:
	struct Glyph {
		float u0, v0, u1, v1; // atlas coordinates
		int width, height, left, top;
		float advance;
	};
	const Glyph *glyph(uint32_t codepoint, int pixel_size, bool bold);
	void set_size(bool bold, int pixel_size);

	GLuint rect_program_ = 0, text_program_ = 0;
	GLint rect_screen_, rect_half_, rect_radius_, rect_color_;
	GLint text_screen_, text_color_, text_atlas_;
	GLuint atlas_ = 0;
	int atlas_size_ = 1024, shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
	std::map<std::tuple<uint32_t, int, bool>, Glyph> glyphs_;
	FT_Library library_ = nullptr;
	FT_Face faces_[2] = {nullptr, nullptr};
	int face_size_[2] = {0, 0};
	float scale_ = 1;
	int width_px_ = 0, height_px_ = 0;
};

} // namespace stardust
