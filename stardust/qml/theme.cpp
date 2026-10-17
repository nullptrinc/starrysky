/* SPDX-License-Identifier: MIT */
#include "theme.h"

#include <QRegularExpression>
#include <QtQml>
#include <algorithm>
#include "constellation-qml.h"

static const char *const appearance_path = "/blinks/system/appearance";

static bool valid_color(const QString &color) {
	static const QRegularExpression rgb(QStringLiteral("^#[0-9a-fA-F]{6}$"));
	return rgb.match(color).hasMatch();
}

SdTheme *SdTheme::instance() {
	static SdTheme *theme = new SdTheme;
	return theme;
}

SdTheme::SdTheme() {
	appearance_ = new CnBlink(this);
	appearance_->setPath(QString::fromLatin1(appearance_path));
	connect(appearance_, &CnBlink::updated, this, &SdTheme::update);
	appearance_->componentComplete(); /* starts watching, as QML would */
}

void SdTheme::update() {
	QString accent = appearance_->values().value(QStringLiteral("accent")).toString();
	accent = valid_color(accent) ? accent.toLower() : QString::fromLatin1(default_accent);
	if (accent != accent_) {
		accent_ = accent;
		emit changed();
	}
}

/* The accent moved toward white, then scaled, as "#AARRGGBB". */
QString SdTheme::mix(float toward_white, float brightness, int alpha) const {
	bool ok = false;
	uint rgb = accent_.mid(1).toUInt(&ok, 16);
	int channels[3] = { int(rgb >> 16 & 0xff), int(rgb >> 8 & 0xff), int(rgb & 0xff) };
	for (int &c : channels) {
		float value = (c + (255 - c) * toward_white) * brightness;
		c = std::clamp(int(value + 0.5f), 0, 255);
	}
	return QStringLiteral("#%1%2%3%4")
		.arg(alpha, 2, 16, QLatin1Char('0'))
		.arg(channels[0], 2, 16, QLatin1Char('0'))
		.arg(channels[1], 2, 16, QLatin1Char('0'))
		.arg(channels[2], 2, 16, QLatin1Char('0'));
}

QString SdTheme::accentLit() const { return mix(0.62f, 1, 255); }
QString SdTheme::accentGlow() const { return mix(0, 1, 0x8c); }
QString SdTheme::accentShade() const { return mix(0, 0.30f, 255); }
QString SdTheme::accentDeep() const { return mix(0, 0.17f, 255); }

bool SdTheme::setAccent(const QString &color) {
	if (!valid_color(color)) {
		qWarning("Theme.setAccent: %s is not #RRGGBB", qPrintable(color));
		return false;
	}
	return appearance_->publish({{QStringLiteral("accent"), color.toLower()}});
}

void registerThemeTypes() {
	qmlRegisterSingletonInstance("Stardust", 1, 0, "Theme", SdTheme::instance());
}
