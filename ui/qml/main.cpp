/* SPDX-License-Identifier: MIT */
/* starry-qml: runs a Starry.UI QML file. The QML engine is only the language;
 * drawing, layout and animation happen in the Starry UI render core. */
#include <QCoreApplication>
#include <QSocketNotifier>
#include <csignal>
#include <unistd.h>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <cstdio>
#include "sui.h"

/* SIGUSR1 taps the first element that handles onClicked, for testing. */
static int tap_pipe[2];

static void on_tap_signal(int) {
	char byte = 1;
	ssize_t ignored = write(tap_pipe[1], &byte, 1);
	(void)ignored;
}

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);
	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE.qml\n", argv[0]);
		return 2;
	}
	qmlRegisterType<SuiWindow>("Starry.UI", 1, 0, "Window");
	qmlRegisterType<SuiNode>("Starry.UI", 1, 0, "Container");
	qmlRegisterType<SuiLabel>("Starry.UI", 1, 0, "Label");

	QQmlEngine engine;
	QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
	QObject *root = component.create();
	if (!root) {
		for (const auto &error : component.errors()) {
			fprintf(stderr, "%s\n", qPrintable(error.toString()));
		}
		return 1;
	}
	auto *window = qobject_cast<SuiWindow *>(root);
	if (!window) {
		fprintf(stderr, "%s: the root element must be a Window\n", argv[1]);
		return 1;
	}
	if (!SuiApp::instance()->start(window)) {
		return 1;
	}
	if (pipe(tap_pipe) == 0) {
		auto *notifier = new QSocketNotifier(tap_pipe[0], QSocketNotifier::Read, &app);
		QObject::connect(notifier, &QSocketNotifier::activated, [] {
			char byte;
			ssize_t ignored = read(tap_pipe[0], &byte, 1);
			(void)ignored;
			SuiApp::instance()->tapFirstClickable();
		});
		signal(SIGUSR1, on_tap_signal);
	}
	return app.exec();
}
