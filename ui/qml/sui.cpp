/* SPDX-License-Identifier: MIT */
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaMethod>
#include <QMetaProperty>
#include <cstdio>
#include "sui.h"

using sui::Mutation;
using sui::Prop;
using sui::Value;

namespace {

/* "#RRGGBB" or "#AARRGGBB"; anything else is transparent. */
sui::Color parse_color(const QString &text) {
	QString hex = text.startsWith('#') ? text.mid(1) : text;
	bool ok = false;
	uint32_t value = hex.toUInt(&ok, 16);
	if (!ok || (hex.size() != 6 && hex.size() != 8)) {
		return {};
	}
	return sui::Color::rgb(hex.size() == 6 ? 0xff000000 | value : value);
}

int parse_enum(const QString &text, std::initializer_list<const char *> names) {
	int index = 0;
	for (const char *name : names) {
		if (text == QLatin1String(name)) {
			return index;
		}
		index++;
	}
	return 0;
}

/* Maps a QML property to the render core's property and value. */
bool convert(const char *name, const QVariant &value, Prop &prop, Value &out) {
	struct Entry {
		const char *name;
		Prop prop;
		enum { Float, Int, Bool, Color, Text, LayoutKind, Direction, Alignment } type;
	};
	static const Entry entries[] = {
		{"background", Prop::Background, Entry::Color},
		{"radius", Prop::Radius, Entry::Float},
		{"opacity", Prop::Opacity, Entry::Float},
		{"scale", Prop::Scale, Entry::Float},
		{"translationX", Prop::TranslationX, Entry::Float},
		{"translationY", Prop::TranslationY, Entry::Float},
		{"layout", Prop::Layout, Entry::LayoutKind},
		{"orientation", Prop::Orientation, Entry::Direction},
		{"spacing", Prop::Spacing, Entry::Float},
		{"padding", Prop::Padding, Entry::Float},
		{"preferredWidth", Prop::PreferredWidth, Entry::Float},
		{"preferredHeight", Prop::PreferredHeight, Entry::Float},
		{"horizontalAlignment", Prop::HorizontalAlign, Entry::Alignment},
		{"verticalAlignment", Prop::VerticalAlign, Entry::Alignment},
		{"animated", Prop::Animated, Entry::Bool},
		{"duration", Prop::Duration, Entry::Int},
		{"text", Prop::Text, Entry::Text},
		{"textColor", Prop::TextColor, Entry::Color},
		{"fontSize", Prop::FontSize, Entry::Float},
		{"bold", Prop::Bold, Entry::Bool},
	};
	for (const Entry &entry : entries) {
		if (qstrcmp(entry.name, name) != 0) {
			continue;
		}
		prop = entry.prop;
		switch (entry.type) {
		case Entry::Float: out = float(value.toDouble()); break;
		case Entry::Int: out = float(value.toInt()); break;
		case Entry::Bool: out = value.toBool() ? 1 : 0; break;
		case Entry::Color: out = parse_color(value.toString()); break;
		case Entry::Text: out = value.toString().toStdString(); break;
		case Entry::LayoutKind: out = parse_enum(value.toString(), {"stack", "dock"}); break;
		case Entry::Direction: out = parse_enum(value.toString(), {"vertical", "horizontal"}); break;
		case Entry::Alignment:
			out = parse_enum(value.toString(), {"fill", "start", "center", "end"});
			break;
		}
		return true;
	}
	return false;
}

} // namespace

/* ---- SuiNode ---- */

SuiNode::SuiNode(QObject *parent) : QObject(parent) {}

QQmlListProperty<SuiNode> SuiNode::nodes() {
	return QQmlListProperty<SuiNode>(this, nullptr, &SuiNode::append, &SuiNode::count,
		&SuiNode::at, nullptr);
}

void SuiNode::append(QQmlListProperty<SuiNode> *list, SuiNode *node) {
	auto *self = static_cast<SuiNode *>(list->object);
	node->parent_node_ = self;
	self->children_.append(node);
}

qsizetype SuiNode::count(QQmlListProperty<SuiNode> *list) {
	return static_cast<SuiNode *>(list->object)->children_.size();
}

SuiNode *SuiNode::at(QQmlListProperty<SuiNode> *list, qsizetype index) {
	return static_cast<SuiNode *>(list->object)->children_.at(index);
}

/* Every property with a change signal reports to one slot. */
void SuiNode::classBegin() {
	const QMetaObject *meta = metaObject();
	const QMetaMethod slot = meta->method(meta->indexOfSlot("propertyChanged()"));
	for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); i++) {
		QMetaProperty property = meta->property(i);
		if (property.hasNotifySignal()) {
			connect(this, property.notifySignal(), this, slot);
		}
	}
}

void SuiNode::propertyChanged() {
	SuiApp *app = SuiApp::instance();
	if (!app->live() || !id) {
		return; // the initial values go out with the node's creation
	}
	int signal = senderSignalIndex();
	const QMetaObject *meta = metaObject();
	for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); i++) {
		QMetaProperty property = meta->property(i);
		if (property.notifySignalIndex() != signal) {
			continue;
		}
		Prop prop;
		Value value;
		if (convert(property.name(), property.read(this), prop, value)) {
			app->post({Mutation::Set, id, 0, sui::Kind::Container, prop, std::move(value)});
		}
	}
}

bool SuiNode::handlesClicked() const {
	return isSignalConnected(QMetaMethod::fromSignal(&SuiNode::clicked));
}

void SuiNode::setPressed(bool pressed) {
	if (pressed_ != pressed) {
		pressed_ = pressed;
		emit pressedChanged();
	}
}

/* ---- SuiTimer ---- */

SuiTimer::SuiTimer(QObject *parent) : QObject(parent) {
	timer_.setInterval(1000);
	timer_.setSingleShot(true);
	connect(&timer_, &QTimer::timeout, this, [this] {
		emit triggered();
		if (timer_.isSingleShot()) {
			emit runningChanged();
		}
	});
}

void SuiTimer::setInterval(int interval) {
	if (interval != timer_.interval()) {
		timer_.setInterval(interval);
		emit intervalChanged();
	}
}

void SuiTimer::setRunning(bool running) {
	if (running == timer_.isActive()) {
		return;
	}
	running ? timer_.start() : timer_.stop();
	emit runningChanged();
}

void SuiTimer::setRepeat(bool repeat) {
	if (repeat == !timer_.isSingleShot()) {
		return;
	}
	timer_.setSingleShot(!repeat);
	emit repeatChanged();
}

void SuiTimer::restart() {
	timer_.start();
	emit runningChanged();
}

/* ---- SuiApp ---- */

SuiApp *SuiApp::instance() {
	static SuiApp app;
	return &app;
}

void SuiApp::create(SuiNode *node, sui::NodeId parent) {
	node->id = ++last_id_;
	nodes_.insert(node->id, node);
	batch_.push_back({Mutation::Create, node->id, 0, node->kind()});
	const QMetaObject *meta = node->metaObject();
	for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); i++) {
		QMetaProperty property = meta->property(i);
		Prop prop;
		Value value;
		if (convert(property.name(), property.read(node), prop, value)) {
			batch_.push_back({Mutation::Set, node->id, 0, sui::Kind::Container, prop,
				std::move(value)});
		}
	}
	batch_.push_back({Mutation::Append, node->id, parent});
	for (SuiNode *child : node->children()) {
		create(child, node->id);
	}
}

bool SuiApp::start(SuiWindow *window) {
	sui::CoreOptions options;
	options.title = window->title_.toStdString();
	options.app_id = window->app_id_.toStdString();
	core_ = std::make_unique<sui::Core>(bridge_, options);
	bridge_.on_event = [this] {
		QMetaObject::invokeMethod(this, &SuiApp::drainEvents, Qt::QueuedConnection);
	};
	if (!core_->start()) {
		return false;
	}
	create(window, 0);
	live_ = true;
	flush();
	return true;
}

void SuiApp::post(Mutation mutation) {
	/* Straight to the render core: a handler that then blocks (or runs long)
	 * must not hold back what it already changed. The core picks up every
	 * mutation waiting at the start of a frame, so a binding cascade still
	 * lands in one frame. */
	bridge_.post(std::move(mutation));
}

void SuiApp::flush() {
	flush_scheduled_ = false;
	if (!batch_.empty()) {
		bridge_.post(std::exchange(batch_, {}));
	}
}

void SuiApp::tapFirstClickable() {
	for (sui::NodeId id = 1; id <= last_id_; id++) {
		SuiNode *node = nodes_.value(id);
		if (node && node->handlesClicked()) {
			QElapsedTimer timer;
			timer.start();
			emit node->clicked();
			fprintf(stderr, "starry-ui trace: node %u onClicked ran %lld ms on the QML thread\n",
				id, (long long)timer.elapsed());
			return;
		}
	}
}

void SuiApp::drainEvents() {
	for (const sui::Event &event : bridge_.take_events()) {
		if (event.type == sui::Event::Closed) {
			QCoreApplication::quit();
			return;
		}
		SuiNode *node = nodes_.value(event.node);
		/* pressed holds for the touched node and all its ancestors. */
		for (SuiNode *n = node; n && event.type != sui::Event::Clicked; n = n->parentNode()) {
			n->setPressed(event.type == sui::Event::Pressed);
		}
		if (event.type == sui::Event::Clicked) {
			/* The nearest ancestor that handles onClicked gets it. */
			for (SuiNode *n = node; n; n = n->parentNode()) {
				if (n->handlesClicked()) {
					emit n->clicked();
					break;
				}
			}
		}
	}
}
