/* SPDX-License-Identifier: MIT */
/*
 * The render core: owns the Wayland window and the GL context on its own
 * thread. It applies mutations from the application, runs layout and
 * implicit animations, turns touches into events, and draws a frame
 * whenever something changed or is still moving.
 */
#pragma once

#include <memory>
#include <string>
#include <thread>
#include "scene.h"

namespace sui {

struct CoreOptions {
	std::string title = "Starry UI";
	std::string app_id = "starry-ui";
	std::string regular_font = "/usr/share/fonts/ttf/LiberationSans-Regular.ttf";
	std::string bold_font = "/usr/share/fonts/ttf/LiberationSans-Bold.ttf";
};

class Core {
public:
	Core(Bridge &bridge, CoreOptions options = {});
	~Core();
	/* Starts the render thread; false if the window could not be created. */
	bool start();
	void stop();

	struct State;

private:
	Bridge &bridge_;
	CoreOptions options_;
	std::unique_ptr<State> state_;
	std::thread thread_;
};

} // namespace sui
