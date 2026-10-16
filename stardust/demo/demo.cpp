/* SPDX-License-Identifier: MIT */
/*
 * Render core demo. The application thread builds a small page and then,
 * on every tap, starts a long slide and immediately blocks for a second.
 * The slide stays smooth: animation runs in the render core, not here.
 */
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include "core.h"

using namespace stardust;

namespace {

class Page {
public:
	explicit Page(Bridge &bridge) : bridge_(bridge) {}

	NodeId create(Kind kind, NodeId parent) {
		NodeId id = ++last_;
		batch_.push_back({Mutation::Create, id, 0, kind});
		batch_.push_back({Mutation::Append, id, parent});
		return id;
	}
	void set(NodeId id, Prop prop, Value value) {
		batch_.push_back({Mutation::Set, id, 0, Kind::Container, prop, std::move(value)});
	}
	void commit() {
		bridge_.post(std::exchange(batch_, {}));
	}

private:
	Bridge &bridge_;
	std::vector<Mutation> batch_;
	NodeId last_ = 0;
};

} // namespace

int main() {
	Bridge bridge;
	Core core(bridge, {.title = "Stardust demo", .app_id = "stardust-demo"});
	if (!core.start()) {
		return 1;
	}

	Page page(bridge);
	NodeId root = page.create(Kind::Container, 0);
	page.set(root, Prop::Layout, int(Layout::Dock));
	page.set(root, Prop::Background, Color::rgb(0xff101425));

	NodeId column = page.create(Kind::Container, root);
	page.set(column, Prop::Padding, 24.0f);
	page.set(column, Prop::Spacing, 16.0f);

	NodeId title = page.create(Kind::Label, column);
	page.set(title, Prop::Text, std::string("Stardust"));
	page.set(title, Prop::FontSize, 30.0f);
	page.set(title, Prop::Bold, 1);

	NodeId note = page.create(Kind::Label, column);
	page.set(note, Prop::Text, std::string("Tap a card: the app blocks for 1 s"));
	page.set(note, Prop::FontSize, 14.0f);
	page.set(note, Prop::TextColor, Color::rgb(0xb3ffffff));

	const uint32_t colours[] = {0xff2f80ed, 0xffeb5757, 0xff27ae60};
	const char *names[] = {"Blue card", "Red card", "Green card"};
	NodeId cards[3], labels[3];
	for (int i = 0; i < 3; i++) {
		cards[i] = page.create(Kind::Container, column);
		page.set(cards[i], Prop::Background, Color::rgb(colours[i]));
		page.set(cards[i], Prop::Radius, 16.0f);
		page.set(cards[i], Prop::Padding, 18.0f);
		labels[i] = page.create(Kind::Label, cards[i]);
		page.set(labels[i], Prop::Text, std::string(names[i]));
		page.set(labels[i], Prop::FontSize, 18.0f);
		page.set(labels[i], Prop::Bold, 1);
	}

	/* A puck that slides across a track on every tap. */
	NodeId track = page.create(Kind::Container, column);
	page.set(track, Prop::Background, Color::rgb(0x33ffffff));
	page.set(track, Prop::Radius, 14.0f);
	page.set(track, Prop::PreferredHeight, 28.0f);
	page.set(track, Prop::Layout, int(Layout::Dock));
	NodeId puck = page.create(Kind::Container, track);
	page.set(puck, Prop::Background, Color::rgb(0xffffffff));
	page.set(puck, Prop::Radius, 14.0f);
	page.set(puck, Prop::PreferredWidth, 28.0f);
	page.set(puck, Prop::PreferredHeight, 28.0f);
	page.set(puck, Prop::HorizontalAlign, int(Align::Start));
	page.set(puck, Prop::Duration, 1500.0f);
	page.commit();

	bool right = false;
	int taps = 0;
	for (;;) {
		for (const Event &event : bridge.wait_events(1000)) {
			if (event.type == Event::Closed) {
				core.stop();
				return 0;
			}
			for (int i = 0; i < 3; i++) {
				bool on_card = event.node == cards[i] || event.node == labels[i];
				if (!on_card) {
					continue;
				}
				if (event.type == Event::Pressed) {
					page.set(cards[i], Prop::Scale, 0.96f);
					page.commit();
				} else if (event.type == Event::Released) {
					page.set(cards[i], Prop::Scale, 1.0f);
					page.commit();
				} else if (event.type == Event::Clicked) {
					right = !right;
					page.set(puck, Prop::TranslationX, right ? 260.0f : 0.0f);
					page.set(title, Prop::Text, "Tapped " + std::to_string(++taps));
					page.commit();
					/* Simulate a busy application thread. */
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
		}
	}
}
