/* SPDX-License-Identifier: MIT */
#include "constellation-qml.h"

#include <QMetaType>
#include <QSocketNotifier>
#include <QTimer>
#include <QtQml>
#include <cstdio>
#include "constellation.h"

static constexpr int RETRY_MSEC = 2000;

CnHub *CnHub::instance() {
	static CnHub *hub = new CnHub;
	return hub;
}

CnHub::CnHub() {
	retry_ = new QTimer(this);
	retry_->setInterval(RETRY_MSEC);
	connect(retry_, &QTimer::timeout, this, &CnHub::connectBroker);
	connectBroker();
}

void CnHub::connectBroker() {
	bool ok = client_ ? cn_reconnect(client_) : (client_ = cn_connect()) != nullptr;
	if (!ok) {
		retry_->start();
		return;
	}
	/* Includes paths watched while the broker was away. */
	for (const QString &path : blinks_.uniqueKeys()) {
		cn_subscribe(client_, path.toUtf8().constData());
	}
	retry_->stop();
	listen();
}

void CnHub::listen() {
	delete notifier_;
	notifier_ = new QSocketNotifier(cn_fd(client_), QSocketNotifier::Read, this);
	connect(notifier_, &QSocketNotifier::activated, this, &CnHub::dispatch);
	/* Requests may have queued events inside the library already. */
	QMetaObject::invokeMethod(this, &CnHub::dispatch, Qt::QueuedConnection);
}

void CnHub::lost() {
	delete notifier_;
	notifier_ = nullptr;
	const QList<CnBlink *> all = blinks_.values();
	for (CnBlink *blink : all) {
		cn_object object;
		cn_object_init(&object, blink->path().toUtf8().constData());
		blink->apply(&object, false);
	}
	retry_->start();
}

void CnHub::changed(const cn_object *object, bool exists, void *data) {
	auto *hub = static_cast<CnHub *>(data);
	for (CnBlink *blink : hub->blinks_.values(QString::fromUtf8(object->path))) {
		blink->apply(object, exists);
	}
}

void CnHub::dispatch() {
	if (client_ && notifier_ && !cn_dispatch(client_, &CnHub::changed, this)) {
		fprintf(stderr, "stardust: constellationd went away\n");
		lost();
	}
}

void CnHub::watch(CnBlink *blink, const QString &path) {
	bool first = !blinks_.contains(path);
	blinks_.insert(path, blink);
	if (!client_ || !notifier_) {
		return;
	}
	if (first) {
		if (!cn_subscribe(client_, path.toUtf8().constData())) {
			fprintf(stderr, "stardust: cannot watch %s\n", qPrintable(path));
		}
		QMetaObject::invokeMethod(this, &CnHub::dispatch, Qt::QueuedConnection);
	} else {
		/* Another object already has the current state; fetch it for this one. */
		cn_object object;
		int found = cn_fetch(client_, path.toUtf8().constData(), &object);
		if (found >= 0) {
			blink->apply(&object, found > 0);
		}
	}
}

void CnHub::unwatch(CnBlink *blink, const QString &path) {
	blinks_.remove(path, blink);
	if (!blinks_.contains(path) && client_ && notifier_) {
		cn_unsubscribe(client_, path.toUtf8().constData());
	}
}

bool CnHub::publish(const QString &path, const QVariantMap &values) {
	if (!client_ || !notifier_) {
		return false;
	}
	cn_object object;
	cn_object_init(&object, path.toUtf8().constData());
	for (auto it = values.begin(); it != values.end(); ++it) {
		QByteArray name = it.key().toUtf8();
		const QVariant &value = it.value();
		switch (value.metaType().id()) {
		case QMetaType::UnknownType:
		case QMetaType::Nullptr:
			cn_set_removed(&object, name.constData());
			break;
		case QMetaType::Bool:
			cn_set_bool(&object, name.constData(), value.toBool());
			break;
		case QMetaType::Int:
		case QMetaType::UInt:
		case QMetaType::LongLong:
		case QMetaType::ULongLong:
		case QMetaType::Double:
		case QMetaType::Float:
			cn_set_number(&object, name.constData(), value.toDouble());
			break;
		default:
			cn_set_string(&object, name.constData(), value.toString().toUtf8().constData());
		}
	}
	bool ok = cn_publish(client_, &object);
	QMetaObject::invokeMethod(this, &CnHub::dispatch, Qt::QueuedConnection);
	return ok;
}

CnBlink::CnBlink(QObject *parent) : QObject(parent) {}

CnBlink::~CnBlink() {
	if (complete_ && !path_.isEmpty()) {
		CnHub::instance()->unwatch(this, path_);
	}
}

void CnBlink::setPath(const QString &path) {
	if (path == path_) {
		return;
	}
	if (complete_ && !path_.isEmpty()) {
		CnHub::instance()->unwatch(this, path_);
	}
	path_ = path;
	emit pathChanged();
	if (complete_ && !path_.isEmpty()) {
		CnHub::instance()->watch(this, path_);
	}
}

void CnBlink::componentComplete() {
	complete_ = true;
	if (!path_.isEmpty()) {
		CnHub::instance()->watch(this, path_);
	}
}

bool CnBlink::publish(const QVariantMap &values) {
	return !path_.isEmpty() && CnHub::instance()->publish(path_, values);
}

void CnBlink::apply(const cn_object *object, bool exists) {
	QVariantMap values;
	for (int i = 0; exists && i < object->count; i++) {
		const cn_attr &attr = object->attrs[i];
		QString name = QString::fromUtf8(attr.name);
		if (attr.type == CN_NUMBER) {
			values.insert(name, cn_get_number(object, attr.name, 0));
		} else if (attr.type == CN_BOOL) {
			values.insert(name, cn_get_bool(object, attr.name, false));
		} else {
			values.insert(name, QString::fromUtf8(attr.value));
		}
	}
	if (exists == exists_ && values == values_) {
		return;
	}
	exists_ = exists;
	values_ = values;
	emit updated();
}

CnBattery::CnBattery(QObject *parent) : CnBlink(parent) {
	setPath(QStringLiteral(CN_NAMESPACE "power/battery"));
}

void registerConstellationTypes() {
	qmlRegisterType<CnBlink>("Constellation", 1, 0, "Blink");
	qmlRegisterType<CnBattery>("Constellation", 1, 0, "Battery");
}
