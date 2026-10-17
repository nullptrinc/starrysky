/* SPDX-License-Identifier: MIT */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "renderer.h"

namespace stardust {

namespace {

/* Positions arrive in device pixels; v_local is relative to the rect centre. */
const char *rect_vertex = R"(
attribute vec2 a_position;
attribute vec2 a_local;
uniform vec2 u_screen;
varying vec2 v_local;
void main() {
	v_local = a_local;
	vec2 clip = a_position / u_screen * 2.0 - 1.0;
	gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
}
)";

/* Signed distance to a rounded box; one pixel of antialiasing. */
const char *rect_fragment = R"(
precision mediump float;
varying vec2 v_local;
uniform vec2 u_half;
uniform float u_radius;
uniform vec4 u_color;
void main() {
	vec2 q = abs(v_local) - u_half + u_radius;
	float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;
	gl_FragColor = u_color * clamp(0.5 - d, 0.0, 1.0);
}
)";

/*
 * Materials, from the same rounded-box distance as plain rects: a gradient
 * fill with optional glass gloss, brushed or chrome surface, border, bevel,
 * inner shadow and glow, then the drop shadow and outer glow around it. All
 * colours are premultiplied. Distances are device pixels; u_scale converts
 * logical sizes.
 */
const char *material_fragment = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v_local;
uniform vec2 u_half;
uniform float u_radius;
uniform vec4 u_top, u_bottom, u_border_color, u_shadow_color, u_glow_color;
uniform float u_border, u_gloss, u_bevel, u_inner_shadow, u_shadow, u_glow, u_inner_glow;
uniform float u_surface, u_light, u_scale, u_alpha;

float box(vec2 p) {
	vec2 q = abs(p) - u_half + u_radius;
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;
}

float hash(float n) {
	return fract(sin(n) * 43758.5453);
}

void main() {
	vec2 p = v_local;
	float d = box(p);
	float coverage = clamp(0.5 - d, 0.0, 1.0);
	float t = clamp((p.y + u_half.y) / (2.0 * u_half.y), 0.0, 1.0);
	vec4 fill = u_bottom.a > 0.0 ? mix(u_top, u_bottom, t) : u_top;

	if (u_surface > 1.5) {
		/* Chrome: a turned-metal reflection around the centre, facing the light. */
		float a = atan(p.y, p.x);
		float lit = 0.5 + 0.5 * cos(2.0 * (a - u_light));
		float rings = 0.5 + 0.5 * cos(6.0 * (a - u_light * 1.3));
		float grain = hash(floor(length(p) / u_scale)) - 0.5;
		float v = 0.30 + 0.62 * lit * lit + 0.10 * rings + 0.04 * grain;
		vec3 tint = fill.a > 0.0 ? fill.rgb / fill.a : vec3(1.0);
		fill.rgb = clamp(v, 0.0, 1.0) * tint * fill.a;
	} else if (u_surface > 0.5) {
		/* Brushed: one streak per device row, broken every few hundred pixels. */
		float row = floor(p.y);
		float n = hash(row + floor(p.x / (160.0 * u_scale) + hash(row) * 3.0) * 17.0);
		fill.rgb += fill.a * (n - 0.5) * 0.035;
	}
	if (u_gloss > 0.0) {
		float upper = 1.0 - step(0.5, t);
		fill.rgb += fill.a * u_gloss * (upper * (0.08 + 0.14 * (1.0 - 2.0 * t)) - (1.0 - upper) * 0.05);
	}
	if (u_border > 0.0) {
		float inner = clamp(0.5 - (d + u_border * u_scale), 0.0, 1.0);
		fill = mix(u_border_color, fill, inner);
	}
	if (u_bevel != 0.0) {
		/* The edge normal of the rounded box; light comes from the top. */
		vec2 q = abs(p) - u_half + u_radius;
		vec2 n = q.x > 0.0 && q.y > 0.0 ? normalize(q) : (q.x > q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
		n *= sign(p);
		float edge = clamp(1.0 + d / (1.5 * u_scale), 0.0, 1.0);
		fill.rgb += fill.a * u_bevel * edge * -n.y * 0.35;
	}
	if (u_inner_shadow > 0.0) {
		float s = u_inner_shadow * u_scale;
		float shade = 1.0 - clamp(-box(p - vec2(0.0, 0.5 * s)) / s, 0.0, 1.0);
		fill.rgb *= 1.0 - 0.85 * shade;
	}
	if (u_inner_glow > 0.0) {
		float g = 1.0 - clamp(-d / (u_inner_glow * u_scale), 0.0, 1.0);
		fill += u_glow_color * g * g * 0.8;
	}
	fill = clamp(fill, 0.0, 1.0);

	vec4 outside = vec4(0.0);
	if (u_shadow > 0.0) {
		float s = u_shadow * u_scale;
		float a = 1.0 - smoothstep(-0.3 * s, s, box(p - vec2(0.0, 0.35 * s)));
		outside = u_shadow_color * a;
	}
	if (u_glow > 0.0) {
		float g = 1.0 - clamp(d / (u_glow * u_scale), 0.0, 1.0);
		g *= g;
		outside = u_glow_color * g + outside * (1.0 - u_glow_color.a * g);
	}
	gl_FragColor = (fill * coverage + outside * (1.0 - coverage)) * u_alpha;
}
)";

const char *text_vertex = R"(
attribute vec2 a_position;
attribute vec2 a_uv;
uniform vec2 u_screen;
uniform vec2 u_offset;
varying vec2 v_uv;
void main() {
	v_uv = a_uv;
	vec2 clip = (a_position + u_offset) / u_screen * 2.0 - 1.0;
	gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
}
)";

const char *text_fragment = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_atlas;
uniform vec4 u_color;
void main() {
	gl_FragColor = u_color * texture2D(u_atlas, v_uv).a;
}
)";

GLuint compile(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		fprintf(stderr, "stardust: shader: %s\n", log);
	}
	return shader;
}

GLuint link(const char *vertex, const char *fragment, const char *second_attribute) {
	GLuint program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER, vertex));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment));
	glBindAttribLocation(program, 0, "a_position");
	glBindAttribLocation(program, 1, second_attribute);
	glLinkProgram(program);
	return program;
}

Color premultiplied(Color c) {
	return {c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}

uint32_t next_codepoint(const char *&s) {
	auto *u = reinterpret_cast<const unsigned char *>(s);
	uint32_t c = *u++;
	int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (extra) {
		c &= 0x3f >> extra;
	}
	while (extra-- && (*u & 0xc0) == 0x80) {
		c = c << 6 | (*u++ & 0x3f);
	}
	s = reinterpret_cast<const char *>(u);
	return c;
}

} // namespace

bool Renderer::init(const char *regular_font, const char *bold_font) {
	if (FT_Init_FreeType(&library_) || FT_New_Face(library_, regular_font, 0, &faces_[0]) ||
			FT_New_Face(library_, bold_font, 0, &faces_[1])) {
		fprintf(stderr, "stardust: cannot load fonts\n");
		return false;
	}
	rect_program_ = link(rect_vertex, rect_fragment, "a_local");
	rect_screen_ = glGetUniformLocation(rect_program_, "u_screen");
	rect_half_ = glGetUniformLocation(rect_program_, "u_half");
	rect_radius_ = glGetUniformLocation(rect_program_, "u_radius");
	rect_color_ = glGetUniformLocation(rect_program_, "u_color");
	material_program_ = link(rect_vertex, material_fragment, "a_local");
	auto uniform = [this](const char *name) { return glGetUniformLocation(material_program_, name); };
	material_ = {
		uniform("u_screen"), uniform("u_half"), uniform("u_radius"), uniform("u_top"),
		uniform("u_bottom"), uniform("u_border_color"), uniform("u_shadow_color"),
		uniform("u_glow_color"), uniform("u_border"), uniform("u_gloss"), uniform("u_bevel"),
		uniform("u_inner_shadow"), uniform("u_shadow"), uniform("u_glow"),
		uniform("u_inner_glow"), uniform("u_surface"), uniform("u_light"), uniform("u_scale"),
		uniform("u_alpha"),
	};
	text_program_ = link(text_vertex, text_fragment, "a_uv");
	text_screen_ = glGetUniformLocation(text_program_, "u_screen");
	text_offset_ = glGetUniformLocation(text_program_, "u_offset");
	text_color_ = glGetUniformLocation(text_program_, "u_color");
	text_atlas_ = glGetUniformLocation(text_program_, "u_atlas");

	atlas_.create();
	glow_atlas_.create();
	return true;
}

void Renderer::begin(int width_px, int height_px, float scale) {
	width_px_ = width_px;
	height_px_ = height_px;
	scale_ = scale;
	glViewport(0, 0, width_px, height_px);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::rect(float x, float y, float w, float h, float radius, Color color) {
	if (color.a <= 0 || w <= 0 || h <= 0) {
		return;
	}
	float s = scale_;
	float x0 = x * s - 1, y0 = y * s - 1, x1 = (x + w) * s + 1, y1 = (y + h) * s + 1;
	float hw = w * s / 2, hh = h * s / 2;
	/* Each vertex: device position, then position relative to the centre. */
	float vertices[] = {
		x0, y0, -hw - 1, -hh - 1, x1, y0, hw + 1, -hh - 1,
		x0, y1, -hw - 1, hh + 1, x1, y1, hw + 1, hh + 1,
	};
	Color c = premultiplied(color);
	glUseProgram(rect_program_);
	glUniform2f(rect_screen_, width_px_, height_px_);
	glUniform2f(rect_half_, hw, hh);
	glUniform1f(rect_radius_, std::fmin(radius * s, std::fmin(hw, hh)));
	glUniform4f(rect_color_, c.r, c.g, c.b, c.a);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, vertices + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::rect(float x, float y, float w, float h, float radius, const Material &m,
		float alpha) {
	if (m.plain()) {
		Color color = m.top;
		color.a *= alpha;
		rect(x, y, w, h, radius, color);
		return;
	}
	if (alpha <= 0 || w <= 0 || h <= 0) {
		return;
	}
	float s = scale_;
	/* Room around the box for the drop shadow and the glow. */
	float margin = std::max(m.shadow * 1.4f, m.glow) * s + 1;
	float x0 = x * s - margin, y0 = y * s - margin;
	float x1 = (x + w) * s + margin, y1 = (y + h) * s + margin;
	float hw = w * s / 2, hh = h * s / 2;
	float lx = hw + margin, ly = hh + margin;
	float vertices[] = {
		x0, y0, -lx, -ly, x1, y0, lx, -ly, x0, y1, -lx, ly, x1, y1, lx, ly,
	};
	auto color = [](GLint location, Color c) {
		Color p = premultiplied(c);
		glUniform4f(location, p.r, p.g, p.b, p.a);
	};
	glUseProgram(material_program_);
	glUniform2f(material_.screen, width_px_, height_px_);
	glUniform2f(material_.half, hw, hh);
	glUniform1f(material_.radius, std::fmin(radius * s, std::fmin(hw, hh)));
	color(material_.top, m.top);
	color(material_.bottom, m.bottom);
	color(material_.border_color, m.border_color);
	color(material_.shadow_color, m.shadow_color);
	color(material_.glow_color, m.glow_color);
	glUniform1f(material_.border, m.border);
	glUniform1f(material_.gloss, m.gloss);
	glUniform1f(material_.bevel, m.bevel);
	glUniform1f(material_.inner_shadow, m.inner_shadow);
	glUniform1f(material_.shadow, m.shadow);
	glUniform1f(material_.glow, m.glow);
	glUniform1f(material_.inner_glow, m.inner_glow);
	glUniform1f(material_.surface, float(m.surface));
	glUniform1f(material_.light, light_);
	glUniform1f(material_.scale, s);
	glUniform1f(material_.alpha, alpha);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, vertices + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Plot coordinates stay inside the node; incomplete/non-finite segments are ignored.
void Renderer::lines(float x, float y, float w, float h,
		const std::vector<float> &points, Color color) {
	if (color.a <= 0 || w <= 0 || h <= 0) return;
	std::vector<float> vertices;
	const size_t end = std::min(points.size(), size_t(16384));
	for (size_t i = 0; i + 3 < end; i += 4) {
		bool finite = true;
		for (size_t j = 0; j < 4; ++j) finite &= std::isfinite(points[i+j]);
		if (!finite) continue;
		for (size_t j = 0; j < 4; j += 2) {
			vertices.push_back((x + std::clamp(points[i+j], 0.f, 1.f) * w) * scale_);
			vertices.push_back((y + std::clamp(points[i+j+1], 0.f, 1.f) * h) * scale_);
		}
	}
	if (vertices.empty()) return;
	Color c = premultiplied(color);
	glUseProgram(rect_program_);
	glUniform2f(rect_screen_, width_px_, height_px_);
	glUniform2f(rect_half_, 2, 2);
	glUniform1f(rect_radius_, 0);
	glUniform4f(rect_color_, c.r, c.g, c.b, c.a);
	glDisableVertexAttribArray(1);
	glVertexAttrib2f(1, 0, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
	glEnableVertexAttribArray(0);
	glLineWidth(scale_);
	glDrawArrays(GL_LINES, 0, vertices.size() / 2);
	glLineWidth(1);
}

void Renderer::set_size(bool bold, int pixel_size) {
	if (face_size_[bold] != pixel_size) {
		FT_Set_Pixel_Sizes(faces_[bold], 0, pixel_size);
		face_size_[bold] = pixel_size;
	}
}

void Renderer::Atlas::create() {
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	std::vector<unsigned char> blank(size * size);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, size, size, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
		blank.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool Renderer::Atlas::place(int w, int h, const unsigned char *pixels, Glyph &glyph) {
	if (shelf_x + w + 1 > size) {
		shelf_x = 0;
		shelf_y += shelf_h + 1;
		shelf_h = 0;
	}
	if (shelf_y + h + 1 > size || w + 1 > size) {
		return false;
	}
	if (w > 0 && h > 0) {
		glBindTexture(GL_TEXTURE_2D, texture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, shelf_x, shelf_y, w, h, GL_ALPHA, GL_UNSIGNED_BYTE,
			pixels);
	}
	float s = size;
	glyph.u0 = shelf_x / s;
	glyph.v0 = shelf_y / s;
	glyph.u1 = (shelf_x + w) / s;
	glyph.v1 = (shelf_y + h) / s;
	glyph.width = w;
	glyph.height = h;
	shelf_x += w + 1;
	shelf_h = std::max(shelf_h, h);
	return true;
}

const Renderer::Glyph *Renderer::glyph(uint32_t codepoint, int pixel_size, bool bold) {
	auto key = std::make_tuple(codepoint, pixel_size, bold);
	auto found = glyphs_.find(key);
	if (found != glyphs_.end()) {
		return &found->second;
	}
	set_size(bold, pixel_size);
	FT_Face face = faces_[bold];
	if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) {
		return nullptr;
	}
	FT_Bitmap &bitmap = face->glyph->bitmap;
	int w = bitmap.width, h = bitmap.rows;
	std::vector<unsigned char> rows(size_t(w) * h);
	for (int row = 0; row < h; row++) {
		std::copy_n(bitmap.buffer + row * bitmap.pitch, w, rows.begin() + size_t(row) * w);
	}
	Glyph glyph{};
	if (!atlas_.place(w, h, rows.data(), glyph)) {
		glyphs_.clear(); /* a full atlas starts over; fine for a phone-sized UI */
		atlas_.reset();
		atlas_.place(w, h, rows.data(), glyph);
	}
	glyph.left = face->glyph->bitmap_left;
	glyph.top = face->glyph->bitmap_top;
	glyph.advance = face->glyph->advance.x / 64.0f;
	return &(glyphs_[key] = glyph);
}

const Renderer::Glyph *Renderer::glow_glyph(uint32_t codepoint, int pixel_size, bool bold,
		int radius) {
	auto key = std::make_tuple(codepoint, pixel_size, bold, radius);
	auto found = glow_glyphs_.find(key);
	if (found != glow_glyphs_.end()) {
		return &found->second;
	}
	set_size(bold, pixel_size);
	FT_Face face = faces_[bold];
	if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) {
		return nullptr;
	}
	FT_Bitmap &bitmap = face->glyph->bitmap;
	int w = bitmap.width, h = bitmap.rows;
	Glyph glyph{};
	glyph.advance = face->glyph->advance.x / 64.0f;
	if (w > 0 && h > 0) {
		/* A separable Gaussian over the glyph padded by the radius. */
		int pad = radius, W = w + 2 * pad, H = h + 2 * pad;
		float sigma = std::max(1.0f, radius / 2.5f);
		std::vector<float> kernel(2 * radius + 1);
		float total = 0;
		for (int i = -radius; i <= radius; i++) {
			total += kernel[i + radius] = std::exp(-0.5f * i * i / (sigma * sigma));
		}
		for (float &k : kernel) {
			k /= total;
		}
		std::vector<float> source(size_t(W) * H), across(size_t(W) * H);
		for (int row = 0; row < h; row++) {
			for (int col = 0; col < w; col++) {
				source[size_t(row + pad) * W + col + pad] = bitmap.buffer[row * bitmap.pitch + col] / 255.0f;
			}
		}
		for (int row = 0; row < H; row++) {
			for (int col = 0; col < W; col++) {
				float sum = 0;
				for (int i = -radius; i <= radius; i++) {
					int c = col + i;
					if (c >= 0 && c < W) {
						sum += source[size_t(row) * W + c] * kernel[i + radius];
					}
				}
				across[size_t(row) * W + col] = sum;
			}
		}
		std::vector<unsigned char> pixels(size_t(W) * H);
		for (int row = 0; row < H; row++) {
			for (int col = 0; col < W; col++) {
				float sum = 0;
				for (int i = -radius; i <= radius; i++) {
					int r = row + i;
					if (r >= 0 && r < H) {
						sum += across[size_t(r) * W + col] * kernel[i + radius];
					}
				}
				/* Blurring lowers the peak; lift it so thin strokes still glow. */
				pixels[size_t(row) * W + col] = (unsigned char)std::lround(std::min(1.0f, sum * 1.6f) * 255);
			}
		}
		if (!glow_atlas_.place(W, H, pixels.data(), glyph)) {
			glow_glyphs_.clear();
			glow_atlas_.reset();
			glow_atlas_.place(W, H, pixels.data(), glyph);
		}
		glyph.left = face->glyph->bitmap_left - pad;
		glyph.top = face->glyph->bitmap_top + pad;
	}
	return &(glow_glyphs_[key] = glyph);
}

template <typename Source>
std::vector<float> Renderer::layout_text(float x, float y, const std::string &text,
		int pixel_size, bool bold, Source source) {
	set_size(bold, pixel_size);
	float baseline = y * scale_ + faces_[bold]->size->metrics.ascender / 64.0f;
	float pen = x * scale_;
	std::vector<float> vertices;
	for (const char *s = text.c_str(); *s;) {
		const Glyph *g = source(next_codepoint(s));
		if (!g) {
			continue;
		}
		float x0 = std::round(pen) + g->left, y0 = std::round(baseline) - g->top;
		float x1 = x0 + g->width, y1 = y0 + g->height;
		if (g->width > 0 && g->height > 0) {
			float quad[] = {
				x0, y0, g->u0, g->v0, x1, y0, g->u1, g->v0, x0, y1, g->u0, g->v1,
				x1, y0, g->u1, g->v0, x1, y1, g->u1, g->v1, x0, y1, g->u0, g->v1,
			};
			vertices.insert(vertices.end(), quad, quad + 24);
		}
		pen += g->advance;
	}
	return vertices;
}

void Renderer::text(float x, float y, const std::string &text, float size, bool bold,
		Color color, const TextStyle &style) {
	if (text.empty() || color.a <= 0) {
		return;
	}
	int pixel_size = std::lround(size * scale_);
	glUseProgram(text_program_);
	glUniform2f(text_screen_, width_px_, height_px_);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(text_atlas_, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	auto draw = [&](const std::vector<float> &vertices, GLuint texture, Color tint, float dy) {
		if (vertices.empty()) {
			return;
		}
		Color c = premultiplied(tint);
		glBindTexture(GL_TEXTURE_2D, texture);
		glUniform4f(text_color_, c.r, c.g, c.b, c.a);
		glUniform2f(text_offset_, 0, dy);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, vertices.data());
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, vertices.data() + 2);
		glDrawArrays(GL_TRIANGLES, 0, vertices.size() / 4);
	};
	/* Backlight: the text blurred once per glyph, drawn under it. */
	int radius = std::min(48, int(std::lround(style.glow * scale_)));
	if (radius > 0 && style.glow_color.a > 0) {
		Color tint = style.glow_color;
		tint.a *= color.a;
		draw(layout_text(x, y, text, pixel_size, bold, [&](uint32_t c) {
			return glow_glyph(c, pixel_size, bold, radius);
		}), glow_atlas_.texture, tint, 0);
	}
	std::vector<float> sharp = layout_text(x, y, text, pixel_size, bold, [&](uint32_t c) {
		return glyph(c, pixel_size, bold);
	});
	if (style.shadow.a > 0) {
		Color tint = style.shadow;
		tint.a *= color.a;
		draw(sharp, atlas_.texture, tint, std::round(style.shadow_offset * scale_));
	}
	draw(sharp, atlas_.texture, color, 0);
}

float Renderer::text_width(const std::string &text, float size, bool bold) {
	int pixel_size = std::lround(size * scale_);
	float width = 0;
	for (const char *s = text.c_str(); *s;) {
		if (const Glyph *g = glyph(next_codepoint(s), pixel_size, bold)) {
			width += g->advance;
		}
	}
	return width / scale_;
}

float Renderer::line_height(float size) {
	return std::ceil(size * 1.25f);
}

} // namespace stardust
