/* SPDX-License-Identifier: MIT */
/* The render core's copy of the node tree: layout and implicit animation. */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "scene.h"

namespace sui {

/* A property that eases to each new value instead of jumping. */
template <typename T>
struct Animated {
	T value{}, from{}, to{};
	double start = 0;
	bool running = false;
};

struct Node {
	NodeId id = 0;
	Kind kind = Kind::Container;
	NodeId parent = 0;
	std::vector<NodeId> children;

	Animated<Color> background;
	float radius = 0;
	Animated<float> opacity, scale, tx, ty;

	std::string text;
	Color text_color = Color::rgb(0xffffffff);
	float font_size = 16;
	bool bold = false;

	Layout layout = Layout::Stack;
	Orientation orientation = Orientation::TopToBottom;
	float spacing = 0, padding = 0;
	float preferred_width = -1, preferred_height = -1;
	Align horizontal = Align::Fill, vertical = Align::Start;
	bool animated = true;
	float duration_ms = 250;
	bool fresh = true; // values set before its first frame apply at once

	float x = 0, y = 0, width = 0, height = 0; // layout result, logical px
};

class TextMetrics {
public:
	virtual ~TextMetrics() = default;
	virtual float text_width(const std::string &text, float size, bool bold) = 0;
	virtual float line_height(float size) = 0;
};

class Tree {
public:
	Tree();
	void apply(const Mutation &mutation, double now);
	/* Advances animations; true while any is still running. */
	bool step(double now);
	void layout(float width, float height, TextMetrics &metrics);
	/* Topmost node under a point, or 0. */
	NodeId hit(float x, float y) const;
	void settle(); // marks every node as drawn once

	Node *get(NodeId id);
	NodeId root() const { return root_; }

private:
	void measure(const Node &node, TextMetrics &metrics, float &width, float &height);
	void arrange(Node &node, float x, float y, float width, float height, TextMetrics &metrics);
	NodeId hit(NodeId id, float x, float y, float ox, float oy) const;

	std::unordered_map<NodeId, Node> nodes_;
	NodeId root_ = 0;
};

} // namespace sui
