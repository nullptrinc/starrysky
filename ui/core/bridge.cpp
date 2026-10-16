/* SPDX-License-Identifier: MIT */
#include <chrono>
#include <utility>
#include "scene.h"

namespace sui {

void Bridge::post(Mutation mutation) {
	{
		std::lock_guard lock(mutex_);
		mutations_.push_back(std::move(mutation));
	}
	if (wake_core) {
		wake_core();
	}
}

void Bridge::post(std::vector<Mutation> mutations) {
	{
		std::lock_guard lock(mutex_);
		for (auto &mutation : mutations) {
			mutations_.push_back(std::move(mutation));
		}
	}
	if (wake_core) {
		wake_core();
	}
}

std::vector<Mutation> Bridge::take_mutations() {
	std::lock_guard lock(mutex_);
	return std::exchange(mutations_, {});
}

void Bridge::emit(Event event) {
	{
		std::lock_guard lock(mutex_);
		events_.push_back(event);
	}
	event_ready_.notify_one();
}

std::vector<Event> Bridge::take_events() {
	std::lock_guard lock(mutex_);
	return std::exchange(events_, {});
}

std::vector<Event> Bridge::wait_events(int timeout_ms) {
	std::unique_lock lock(mutex_);
	event_ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
		[this] { return !events_.empty(); });
	return std::exchange(events_, {});
}

} // namespace sui
