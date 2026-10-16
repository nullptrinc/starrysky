/* SPDX-License-Identifier: MIT */
#include <algorithm>
#include <cmath>
#include "tree.h"

namespace stardust {

namespace {

float ease_out(double t) {
	return 1 - std::pow(1 - t, 3);
}

float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

Color lerp(const Color &a, const Color &b, float t) {
	return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), lerp(a.a, b.a, t)};
}

template <typename T>
void set_animated(Node &node, Animated<T> &property, const T &target, double now) {
	if (node.fresh || !node.animated || node.duration_ms <= 0) {
		property.value = property.from = property.to = target;
		property.running = false;
		return;
	}
	property.from = property.value; // retarget smoothly from wherever it is now
	property.to = target;
	property.start = now;
	property.running = true;
}

template <typename T>
bool advance(Animated<T> &property, double now, float duration_ms) {
	if (!property.running) {
		return false;
	}
	double t = std::min(1.0, (now - property.start) * 1000 / duration_ms);
	property.value = lerp(property.from, property.to, ease_out(t));
	if (t >= 1) {
		property.value = property.to;
		property.running = false;
	}
	return property.running;
}

float as_float(const Value &value) {
	if (auto *f = std::get_if<float>(&value)) {
		return *f;
	}
	if (auto *i = std::get_if<int>(&value)) {
		return float(*i);
	}
	return 0;
}

int as_int(const Value &value) {
	if (auto *i = std::get_if<int>(&value)) {
		return *i;
	}
	return int(as_float(value));
}

} // namespace

Tree::Tree() = default;

Node *Tree::get(NodeId id) {
	auto found = nodes_.find(id);
	return found == nodes_.end() ? nullptr : &found->second;
}

void Tree::apply(const Mutation &mutation, double now) {
	switch (mutation.op) {
	case Mutation::Create: {
		Node node;
		node.id = mutation.node;
		node.kind = mutation.kind;
		node.opacity.value = node.opacity.to = 1;
		node.scale.value = node.scale.to = 1;
		nodes_[node.id] = std::move(node);
		return;
	}
	case Mutation::Destroy: {
		Node *node = get(mutation.node);
		if (!node) {
			return;
		}
		std::vector<NodeId> children = node->children;
		if (Node *parent = get(node->parent)) {
			auto &list = parent->children;
			list.erase(std::remove(list.begin(), list.end(), node->id), list.end());
		}
		if (root_ == mutation.node) {
			root_ = 0;
		}
		nodes_.erase(mutation.node);
		for (NodeId child : children) {
			apply({Mutation::Destroy, child}, now);
		}
		return;
	}
	case Mutation::Append: {
		Node *node = get(mutation.node);
		if (!node) {
			return;
		}
		if (mutation.parent == 0) {
			root_ = node->id;
			return;
		}
		if (Node *parent = get(mutation.parent)) {
			node->parent = parent->id;
			parent->children.push_back(node->id);
		}
		return;
	}
	case Mutation::Set:
		break;
	}

	Node *node = get(mutation.node);
	if (!node) {
		return;
	}
	const Value &value = mutation.value;
	switch (mutation.prop) {
	case Prop::Background:
		if (auto *color = std::get_if<Color>(&value)) {
			set_animated(*node, node->background, *color, now);
		}
		break;
	case Prop::Radius: node->radius = as_float(value); break;
	case Prop::Opacity: set_animated(*node, node->opacity, as_float(value), now); break;
	case Prop::Scale: set_animated(*node, node->scale, as_float(value), now); break;
	case Prop::TranslationX: set_animated(*node, node->tx, as_float(value), now); break;
	case Prop::TranslationY: set_animated(*node, node->ty, as_float(value), now); break;
	case Prop::Text:
		if (auto *text = std::get_if<std::string>(&value)) {
			node->text = *text;
		}
		break;
	case Prop::TextColor:
		if (auto *color = std::get_if<Color>(&value)) {
			node->text_color = *color;
		}
		break;
	case Prop::Points:
		if (auto *points = std::get_if<std::vector<float>>(&value)) node->points = *points;
		break;
	case Prop::FontSize: node->font_size = as_float(value); break;
	case Prop::Bold: node->bold = as_int(value) != 0; break;
	case Prop::Layout: node->layout = Layout(as_int(value)); break;
	case Prop::Orientation: node->orientation = Orientation(as_int(value)); break;
	case Prop::Spacing: node->spacing = as_float(value); break;
	case Prop::Padding: node->padding = as_float(value); break;
	case Prop::PreferredWidth: node->preferred_width = as_float(value); break;
	case Prop::PreferredHeight: node->preferred_height = as_float(value); break;
	case Prop::HorizontalAlign: node->horizontal = Align(as_int(value)); break;
	case Prop::VerticalAlign: node->vertical = Align(as_int(value)); break;
	case Prop::Animated: node->animated = as_int(value) != 0; break;
	case Prop::Duration: node->duration_ms = as_float(value); break;
	}
}

bool Tree::step(double now) {
	bool running = false;
	for (auto &[id, node] : nodes_) {
		running |= advance(node.background, now, node.duration_ms);
		running |= advance(node.opacity, now, node.duration_ms);
		running |= advance(node.scale, now, node.duration_ms);
		running |= advance(node.tx, now, node.duration_ms);
		running |= advance(node.ty, now, node.duration_ms);
	}
	return running;
}

void Tree::settle() {
	for (auto &[id, node] : nodes_) {
		node.fresh = false;
	}
}

/* ---- layout: StackLayout and DockLayout, as in Cascades ---- */

void Tree::measure(const Node &node, TextMetrics &metrics, float &width, float &height) {
	width = height = 0;
	if (node.kind == Kind::Label) {
		width = metrics.text_width(node.text, node.font_size, node.bold);
		height = metrics.line_height(node.font_size);
	} else {
		bool vertical = node.orientation == Orientation::TopToBottom;
		size_t count = 0;
		for (NodeId id : node.children) {
			auto found = nodes_.find(id);
			if (found == nodes_.end()) {
				continue;
			}
			float w, h;
			measure(found->second, metrics, w, h);
			if (node.layout == Layout::Dock) {
				width = std::max(width, w);
				height = std::max(height, h);
			} else if (vertical) {
				width = std::max(width, w);
				height += h;
			} else {
				width += w;
				height = std::max(height, h);
			}
			count++;
		}
		if (node.layout == Layout::Stack && count > 1) {
			(vertical ? height : width) += node.spacing * (count - 1);
		}
		width += 2 * node.padding;
		height += 2 * node.padding;
	}
	if (node.preferred_width >= 0) {
		width = node.preferred_width;
	}
	if (node.preferred_height >= 0) {
		height = node.preferred_height;
	}
}

static float place(Align align, float start, float available, float size) {
	switch (align) {
	case Align::Center: return start + (available - size) / 2;
	case Align::End: return start + available - size;
	default: return start;
	}
}

void Tree::arrange(Node &node, float x, float y, float width, float height,
		TextMetrics &metrics) {
	node.x = x;
	node.y = y;
	node.width = width;
	node.height = height;
	float inner_x = x + node.padding, inner_y = y + node.padding;
	float inner_w = std::max(0.0f, width - 2 * node.padding);
	float inner_h = std::max(0.0f, height - 2 * node.padding);
	float cursor = node.orientation == Orientation::TopToBottom ? inner_y : inner_x;

	for (NodeId id : node.children) {
		auto found = nodes_.find(id);
		if (found == nodes_.end()) {
			continue;
		}
		Node &child = found->second;
		float w, h;
		measure(child, metrics, w, h);
		if (child.horizontal == Align::Fill) {
			w = node.layout == Layout::Stack && node.orientation == Orientation::LeftToRight ?
				w : inner_w;
		}
		if (child.vertical == Align::Fill) {
			h = node.layout == Layout::Stack && node.orientation == Orientation::TopToBottom ?
				h : inner_h;
		}
		if (node.layout == Layout::Dock) {
			arrange(child, place(child.horizontal, inner_x, inner_w, w),
				place(child.vertical, inner_y, inner_h, h), w, h, metrics);
		} else if (node.orientation == Orientation::TopToBottom) {
			arrange(child, place(child.horizontal, inner_x, inner_w, w), cursor, w, h, metrics);
			cursor += h + node.spacing;
		} else {
			arrange(child, cursor, place(child.vertical, inner_y, inner_h, h), w, h, metrics);
			cursor += w + node.spacing;
		}
	}
}

void Tree::layout(float width, float height, TextMetrics &metrics) {
	if (Node *root = get(root_)) {
		arrange(*root, 0, 0, width, height, metrics);
	}
}

/* ---- touch: deepest node under the point, honouring translations ---- */

NodeId Tree::hit(NodeId id, float x, float y, float ox, float oy) const {
	auto found = nodes_.find(id);
	if (found == nodes_.end()) {
		return 0;
	}
	const Node &node = found->second;
	ox += node.tx.value;
	oy += node.ty.value;
	float left = node.x + ox, top = node.y + oy;
	if (x < left || y < top || x >= left + node.width || y >= top + node.height ||
			node.opacity.value <= 0.01f) {
		return 0;
	}
	for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
		if (NodeId found_child = hit(*child, x, y, ox, oy)) {
			return found_child;
		}
	}
	return node.id;
}

NodeId Tree::hit(float x, float y) const {
	return hit(root_, x, y, 0, 0);
}

} // namespace stardust
