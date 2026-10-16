/* SPDX-License-Identifier: MIT */
/*
 * Constellation for QML: "import Constellation 1.0".
 *
 *     property Battery battery: Battery {}
 *     property Blink saver: Blink { path: "/blinks/power/saver" }
 *     Label { text: battery.level + "%" }
 *     Label { text: saver.values.enabled ? "Saver on" : "" }
 *
 * Every object in a process shares one connection to constellationd, which
 * is restored when the broker restarts.
 */
#pragma once

#include <QMultiHash>
#include <QObject>
#include <QQmlParserStatus>
#include <QString>
#include <QVariantMap>

struct cn_client;
struct cn_object;
class QSocketNotifier;
class QTimer;
class CnBlink;

/* The process's connection to constellationd. */
class CnHub : public QObject {
	Q_OBJECT

public:
	static CnHub *instance();
	void watch(CnBlink *blink, const QString &path);
	void unwatch(CnBlink *blink, const QString &path);
	bool publish(const QString &path, const QVariantMap &values);

private slots:
	void dispatch();
	void connectBroker();

private:
	CnHub();
	void listen();
	void lost();
	static void changed(const cn_object *object, bool exists, void *data);

	cn_client *client_ = nullptr;
	QSocketNotifier *notifier_ = nullptr;
	QTimer *retry_ = nullptr;
	QMultiHash<QString, CnBlink *> blinks_;
};

/* One object of the /blinks namespace. */
class CnBlink : public QObject, public QQmlParserStatus {
	Q_OBJECT
	Q_INTERFACES(QQmlParserStatus)
	Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
	Q_PROPERTY(bool exists READ exists NOTIFY updated)
	Q_PROPERTY(QVariantMap values READ values NOTIFY updated)

public:
	explicit CnBlink(QObject *parent = nullptr);
	~CnBlink() override;

	QString path() const { return path_; }
	void setPath(const QString &path);
	bool exists() const { return exists_; }
	QVariantMap values() const { return values_; }

	/* Merges values into the object; a null value removes the attribute. The
	 * first process to publish an object owns it until it exits. */
	Q_INVOKABLE bool publish(const QVariantMap &values);

	void apply(const cn_object *object, bool exists);
	void classBegin() override {}
	void componentComplete() override;

signals:
	void pathChanged();
	void updated();

private:
	QString path_;
	bool exists_ = false;
	bool complete_ = false;
	QVariantMap values_;
};

/* /blinks/power/battery, as typed properties. */
class CnBattery : public CnBlink {
	Q_OBJECT
	Q_PROPERTY(bool present READ present NOTIFY updated)
	Q_PROPERTY(int level READ level NOTIFY updated)
	Q_PROPERTY(QString status READ status NOTIFY updated)
	Q_PROPERTY(bool charging READ charging NOTIFY updated)
	Q_PROPERTY(bool plugged READ plugged NOTIFY updated)
	Q_PROPERTY(qreal voltage READ voltage NOTIFY updated)
	Q_PROPERTY(qreal current READ current NOTIFY updated)
	Q_PROPERTY(qreal temperature READ temperature NOTIFY updated)

public:
	explicit CnBattery(QObject *parent = nullptr);
	bool present() const { return values().value("present").toBool(); }
	int level() const { return values().value("level").toInt(); }
	QString status() const { return values().value("status").toString(); }
	bool charging() const { return values().value("charging").toBool(); }
	bool plugged() const { return values().value("plugged").toBool(); }
	qreal voltage() const { return values().value("voltage").toDouble(); }
	qreal current() const { return values().value("current").toDouble(); }
	qreal temperature() const { return values().value("temperature").toDouble(); }
};

void registerConstellationTypes();
