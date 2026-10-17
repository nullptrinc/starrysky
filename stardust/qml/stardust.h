/* SPDX-License-Identifier: MIT */
/*
 * Stardust QML types. They are thin QObjects: every property change becomes
 * a mutation for the render core, which does the layout, animation and
 * drawing on its own thread. Touches come back as signals.
 */
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QQmlListProperty>
#include <QQmlParserStatus>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <memory>
#include <vector>
#include "core.h"
#include "scene.h"

class SdNode : public QObject, public QQmlParserStatus {
	Q_OBJECT
	Q_INTERFACES(QQmlParserStatus)
	Q_CLASSINFO("DefaultProperty", "nodes")

	Q_PROPERTY(QString background MEMBER background_ NOTIFY backgroundChanged)
	Q_PROPERTY(qreal radius MEMBER radius_ NOTIFY radiusChanged)
	Q_PROPERTY(qreal opacity MEMBER opacity_ NOTIFY opacityChanged)
	Q_PROPERTY(qreal scale MEMBER scale_ NOTIFY scaleChanged)
	Q_PROPERTY(qreal translationX MEMBER translation_x_ NOTIFY translationXChanged)
	Q_PROPERTY(qreal translationY MEMBER translation_y_ NOTIFY translationYChanged)
	Q_PROPERTY(QString layout MEMBER layout_ NOTIFY layoutChanged)
	Q_PROPERTY(QString orientation MEMBER orientation_ NOTIFY orientationChanged)
	Q_PROPERTY(qreal spacing MEMBER spacing_ NOTIFY spacingChanged)
	Q_PROPERTY(qreal padding MEMBER padding_ NOTIFY paddingChanged)
	Q_PROPERTY(qreal preferredWidth MEMBER preferred_width_ NOTIFY preferredWidthChanged)
	Q_PROPERTY(qreal preferredHeight MEMBER preferred_height_ NOTIFY preferredHeightChanged)
	Q_PROPERTY(QString horizontalAlignment MEMBER horizontal_ NOTIFY horizontalAlignmentChanged)
	Q_PROPERTY(QString verticalAlignment MEMBER vertical_ NOTIFY verticalAlignmentChanged)
	Q_PROPERTY(bool animated MEMBER animated_ NOTIFY animatedChanged)
	Q_PROPERTY(int duration MEMBER duration_ NOTIFY durationChanged)
	Q_PROPERTY(bool pressed READ pressed NOTIFY pressedChanged)
	Q_PROPERTY(QQmlListProperty<SdNode> nodes READ nodes)
	/* Materials: background runs to backgroundEnd as a vertical gradient. */
	Q_PROPERTY(QString backgroundEnd MEMBER background_end_ NOTIFY backgroundEndChanged)
	Q_PROPERTY(qreal gloss MEMBER gloss_ NOTIFY glossChanged)
	Q_PROPERTY(qreal borderWidth MEMBER border_width_ NOTIFY borderWidthChanged)
	Q_PROPERTY(QString borderColor MEMBER border_color_ NOTIFY borderColorChanged)
	Q_PROPERTY(qreal bevel MEMBER bevel_ NOTIFY bevelChanged)
	Q_PROPERTY(qreal innerShadow MEMBER inner_shadow_ NOTIFY innerShadowChanged)
	Q_PROPERTY(qreal shadow MEMBER shadow_ NOTIFY shadowChanged)
	Q_PROPERTY(QString shadowColor MEMBER shadow_color_ NOTIFY shadowColorChanged)
	Q_PROPERTY(qreal glow MEMBER glow_ NOTIFY glowChanged)
	Q_PROPERTY(qreal innerGlow MEMBER inner_glow_ NOTIFY innerGlowChanged)
	Q_PROPERTY(QString glowColor MEMBER glow_color_ NOTIFY glowColorChanged)
	Q_PROPERTY(QString material MEMBER material_ NOTIFY materialChanged)

public:
	explicit SdNode(QObject *parent = nullptr);
	virtual stardust::Kind kind() const { return stardust::Kind::Container; }

	QQmlListProperty<SdNode> nodes();
	const QList<SdNode *> &children() const { return children_; }
	SdNode *parentNode() const { return parent_node_; }
	bool pressed() const { return pressed_; }
	void setPressed(bool pressed);
	/* Whether QML code handles onClicked on this node. */
	bool handlesClicked() const;

	stardust::NodeId id = 0;

	void classBegin() override;
	void componentComplete() override {}

signals:
	void backgroundChanged();
	void radiusChanged();
	void opacityChanged();
	void scaleChanged();
	void translationXChanged();
	void translationYChanged();
	void layoutChanged();
	void orientationChanged();
	void spacingChanged();
	void paddingChanged();
	void preferredWidthChanged();
	void preferredHeightChanged();
	void horizontalAlignmentChanged();
	void verticalAlignmentChanged();
	void animatedChanged();
	void durationChanged();
	void pressedChanged(); /* also the way to react to touch down and up */
	void clicked();
	void backgroundEndChanged();
	void glossChanged();
	void borderWidthChanged();
	void borderColorChanged();
	void bevelChanged();
	void innerShadowChanged();
	void shadowChanged();
	void shadowColorChanged();
	void glowChanged();
	void innerGlowChanged();
	void glowColorChanged();
	void materialChanged();

private slots:
	void propertyChanged();

private:
	static void append(QQmlListProperty<SdNode> *list, SdNode *node);
	static qsizetype count(QQmlListProperty<SdNode> *list);
	static SdNode *at(QQmlListProperty<SdNode> *list, qsizetype index);

	QString background_;
	qreal radius_ = 0, opacity_ = 1, scale_ = 1, translation_x_ = 0, translation_y_ = 0;
	QString layout_ = QStringLiteral("stack"), orientation_ = QStringLiteral("vertical");
	qreal spacing_ = 0, padding_ = 0, preferred_width_ = -1, preferred_height_ = -1;
	QString horizontal_ = QStringLiteral("fill"), vertical_ = QStringLiteral("start");
	bool animated_ = true;
	int duration_ = 250;
	bool pressed_ = false;
	QString background_end_, border_color_, glow_color_;
	QString shadow_color_ = QStringLiteral("#cc000000");
	QString material_ = QStringLiteral("plain");
	qreal gloss_ = 0, border_width_ = 0, bevel_ = 0, inner_shadow_ = 0, shadow_ = 0;
	qreal glow_ = 0, inner_glow_ = 0;
	QList<SdNode *> children_;
	SdNode *parent_node_ = nullptr;
};

class SdLabel : public SdNode {
	Q_OBJECT
	Q_PROPERTY(QString text MEMBER text_ NOTIFY textChanged)
	Q_PROPERTY(QString textColor MEMBER text_color_ NOTIFY textColorChanged)
	Q_PROPERTY(qreal fontSize MEMBER font_size_ NOTIFY fontSizeChanged)
	Q_PROPERTY(bool bold MEMBER bold_ NOTIFY boldChanged)
	Q_PROPERTY(QString textShadow MEMBER text_shadow_ NOTIFY textShadowChanged)
	Q_PROPERTY(qreal textShadowOffset MEMBER text_shadow_offset_ NOTIFY textShadowOffsetChanged)
	Q_PROPERTY(qreal textGlow MEMBER text_glow_ NOTIFY textGlowChanged)
	Q_PROPERTY(QString textGlowColor MEMBER text_glow_color_ NOTIFY textGlowColorChanged)

public:
	using SdNode::SdNode;
	stardust::Kind kind() const override { return stardust::Kind::Label; }

signals:
	void textChanged();
	void textColorChanged();
	void fontSizeChanged();
	void boldChanged();
	void textShadowChanged();
	void textShadowOffsetChanged();
	void textGlowChanged();
	void textGlowColorChanged();

private:
	QString text_shadow_, text_glow_color_;
	qreal text_shadow_offset_ = 1, text_glow_ = 0;
	QString text_, text_color_ = QStringLiteral("#ffffff");
	qreal font_size_ = 16;
	bool bold_ = false;
};

/* Batched line segments in normalized coordinates, rendered on the core thread. */
class SdPlot : public SdNode {
	Q_OBJECT
	Q_PROPERTY(QVariantList points MEMBER points_ NOTIFY pointsChanged)
	Q_PROPERTY(QString textColor MEMBER color_ NOTIFY colorChanged)
public:
	using SdNode::SdNode;
	stardust::Kind kind() const override { return stardust::Kind::Plot; }
signals:
	void pointsChanged();
	void colorChanged();
private:
	QVariantList points_;
	QString color_ = QStringLiteral("#ffffff");
};

/* The top-level element: one application window. */
class SdWindow : public SdNode {
	Q_OBJECT
	Q_PROPERTY(QString title MEMBER title_ NOTIFY titleChanged)
	Q_PROPERTY(QString appId MEMBER app_id_ NOTIFY appIdChanged)
	/* Where chrome surfaces see the light, in degrees; the tilt will drive it. */
	Q_PROPERTY(qreal lightAngle MEMBER light_angle_ NOTIFY lightAngleChanged)

public:
	using SdNode::SdNode;
	QString title_ = QStringLiteral("Starry"), app_id_ = QStringLiteral("stardust");
	qreal light_angle_ = 200;

signals:
	void titleChanged();
	void appIdChanged();
	void lightAngleChanged();
};

/* A repeating or one-shot timer, so apps need no extra QML modules. */
class SdTimer : public QObject {
	Q_OBJECT
	Q_PROPERTY(int interval READ interval WRITE setInterval NOTIFY intervalChanged)
	Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
	Q_PROPERTY(bool repeat READ repeat WRITE setRepeat NOTIFY repeatChanged)

public:
	explicit SdTimer(QObject *parent = nullptr);
	int interval() const { return timer_.interval(); }
	void setInterval(int interval);
	bool running() const { return timer_.isActive(); }
	void setRunning(bool running);
	bool repeat() const { return !timer_.isSingleShot(); }
	void setRepeat(bool repeat);
	Q_INVOKABLE void restart();

signals:
	void intervalChanged();
	void runningChanged();
	void repeatChanged();
	void triggered();

private:
	QTimer timer_;
};

/* Owns the render core and translates between QML objects and the bridge. */
class SdApp : public QObject {
	Q_OBJECT
public:
	static SdApp *instance();
	bool start(SdWindow *window);
	void stop();
	void post(stardust::Mutation mutation);
	bool live() const { return live_; }
	/* Emits clicked on the first node with an onClicked handler. */
	void tapFirstClickable();

private slots:
	void flush();
	void drainEvents();

private:
	void create(SdNode *node, stardust::NodeId parent);

	stardust::Bridge bridge_;
	std::unique_ptr<stardust::Core> core_;
	std::vector<stardust::Mutation> batch_;
	QHash<stardust::NodeId, SdNode *> nodes_;
	stardust::NodeId last_id_ = 0;
	bool flush_scheduled_ = false, live_ = false;
};
