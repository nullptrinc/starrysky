/* SPDX-License-Identifier: MIT */
/*
 * Starry UI: the application thread describes a tree of nodes; the render
 * core owns a mirror of it on its own thread and does layout, implicit
 * animation, touch handling and drawing. The two sides only exchange
 * mutations and events, so a busy application never stalls an animation,
 * the way Cascades worked on BlackBerry 10.
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace sui {

using NodeId = uint32_t;

enum class Kind : uint8_t { Container, Label };
enum class Layout : uint8_t { Stack, Dock };
enum class Orientation : uint8_t { TopToBottom, LeftToRight };
enum class Align : uint8_t { Fill, Start, Center, End };

struct Color {
	float r = 0, g = 0, b = 0, a = 0;
	static Color rgb(uint32_t argb) {
		return {((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
			(argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f};
	}
};

enum class Prop : uint8_t {
	Background, Radius, Opacity, Scale, TranslationX, TranslationY,
	Text, TextColor, FontSize, Bold,
	Layout, Orientation, Spacing, Padding,
	PreferredWidth, PreferredHeight, HorizontalAlign, VerticalAlign,
	Animated, // 0: changes apply at once
	Duration, // implicit animation length, ms (default 250)
};

using Value = std::variant<float, Color, std::string, int>;

struct Mutation {
	enum Op : uint8_t { Create, Destroy, Append, Set } op;
	NodeId node = 0;
	NodeId parent = 0; // Append
	Kind kind = Kind::Container; // Create
	Prop prop = Prop::Opacity; // Set
	Value value;
};

struct Event {
	enum Type : uint8_t { Pressed, Released, Clicked, Closed } type;
	NodeId node = 0;
	float x = 0, y = 0;
};

/* The only state shared between the application and render threads. */
class Bridge {
public:
	void post(Mutation mutation);
	void post(std::vector<Mutation> mutations);
	std::vector<Mutation> take_mutations();

	void emit(Event event);
	std::vector<Event> take_events();
	/* Blocks the application thread until an event or the timeout. */
	std::vector<Event> wait_events(int timeout_ms);

	std::function<void()> wake_core; // set by the render core

private:
	std::mutex mutex_;
	std::vector<Mutation> mutations_;
	std::vector<Event> events_;
	std::condition_variable event_ready_;
};

} // namespace sui
