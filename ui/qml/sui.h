/* SPDX-License-Identifier: MIT */
/*
 * Starry.UI QML types. They are thin QObjects: every property change becomes
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
#include <memory>
#include <vector>
#include "core.h"
#include "scene.h"

class SuiNode : public QObject, public QQmlParserStatus {
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
	Q_PROPERTY(QQmlListProperty<SuiNode> nodes READ nodes)

public:
	explicit SuiNode(QObject *parent = nullptr);
	virtual sui::Kind kind() const { return sui::Kind::Container; }

	QQmlListProperty<SuiNode> nodes();
	const QList<SuiNode *> &children() const { return children_; }
	SuiNode *parentNode() const { return parent_node_; }
	bool pressed() const { return pressed_; }
	void setPressed(bool pressed);
	/* Whether QML code handles onClicked on this node. */
	bool handlesClicked() const;

	sui::NodeId id = 0;

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

private slots:
	void propertyChanged();

private:
	static void append(QQmlListProperty<SuiNode> *list, SuiNode *node);
	static qsizetype count(QQmlListProperty<SuiNode> *list);
	static SuiNode *at(QQmlListProperty<SuiNode> *list, qsizetype index);

	QString background_;
	qreal radius_ = 0, opacity_ = 1, scale_ = 1, translation_x_ = 0, translation_y_ = 0;
	QString layout_ = QStringLiteral("stack"), orientation_ = QStringLiteral("vertical");
	qreal spacing_ = 0, padding_ = 0, preferred_width_ = -1, preferred_height_ = -1;
	QString horizontal_ = QStringLiteral("fill"), vertical_ = QStringLiteral("start");
	bool animated_ = true;
	int duration_ = 250;
	bool pressed_ = false;
	QList<SuiNode *> children_;
	SuiNode *parent_node_ = nullptr;
};

class SuiLabel : public SuiNode {
	Q_OBJECT
	Q_PROPERTY(QString text MEMBER text_ NOTIFY textChanged)
	Q_PROPERTY(QString textColor MEMBER text_color_ NOTIFY textColorChanged)
	Q_PROPERTY(qreal fontSize MEMBER font_size_ NOTIFY fontSizeChanged)
	Q_PROPERTY(bool bold MEMBER bold_ NOTIFY boldChanged)

public:
	using SuiNode::SuiNode;
	sui::Kind kind() const override { return sui::Kind::Label; }

signals:
	void textChanged();
	void textColorChanged();
	void fontSizeChanged();
	void boldChanged();

private:
	QString text_, text_color_ = QStringLiteral("#ffffff");
	qreal font_size_ = 16;
	bool bold_ = false;
};

/* The top-level element: one application window. */
class SuiWindow : public SuiNode {
	Q_OBJECT
	Q_PROPERTY(QString title MEMBER title_ NOTIFY titleChanged)
	Q_PROPERTY(QString appId MEMBER app_id_ NOTIFY appIdChanged)

public:
	using SuiNode::SuiNode;
	QString title_ = QStringLiteral("Starry"), app_id_ = QStringLiteral("starry-qml");

signals:
	void titleChanged();
	void appIdChanged();
};

/* Owns the render core and translates between QML objects and the bridge. */
class SuiApp : public QObject {
	Q_OBJECT
public:
	static SuiApp *instance();
	bool start(SuiWindow *window);
	void post(sui::Mutation mutation);
	bool live() const { return live_; }
	/* Emits clicked on the first node with an onClicked handler. */
	void tapFirstClickable();

private slots:
	void flush();
	void drainEvents();

private:
	void create(SuiNode *node, sui::NodeId parent);

	sui::Bridge bridge_;
	std::unique_ptr<sui::Core> core_;
	std::vector<sui::Mutation> batch_;
	QHash<sui::NodeId, SuiNode *> nodes_;
	sui::NodeId last_id_ = 0;
	bool flush_scheduled_ = false, live_ = false;
};
