/* SPDX-License-Identifier: MIT */
#include <EGL/egl.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "core.h"
#include "renderer.h"
#include "tree.h"
#include "xdg-shell-client-protocol.h"

namespace sui {

namespace {

constexpr int max_touch = 10;
constexpr float tap_slop = 12;

double now_seconds() {
	using clock = std::chrono::steady_clock;
	return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

} // namespace

struct Core::State {
	Bridge &bridge;
	CoreOptions options;
	std::atomic<bool> running{true};
	int wake_fd = -1;

	wl_display *display = nullptr;
	wl_compositor *compositor = nullptr;
	xdg_wm_base *wm_base = nullptr;
	wl_seat *seat = nullptr;
	wl_touch *touch = nullptr;
	wl_output *output = nullptr;
	wl_surface *surface = nullptr;
	xdg_surface *window = nullptr;
	xdg_toplevel *toplevel = nullptr;
	wl_egl_window *egl_window = nullptr;
	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;
	EGLSurface egl_surface = EGL_NO_SURFACE;

	int width = 360, height = 640, scale = 1;
	bool configured = false, resized = false, dirty = true;

	Tree tree;
	Renderer renderer;

	struct Touch {
		bool down = false;
		NodeId node = 0;
		float start_x = 0, start_y = 0, x = 0, y = 0;
	} touches[max_touch];

	State(Bridge &bridge, CoreOptions options) : bridge(bridge), options(std::move(options)) {}

	bool connect();
	bool create_gl();
	void run();
	void draw(NodeId id, float alpha, float k, float tx, float ty);
	void frame();
	void teardown();
};

/* ---- Wayland listeners ---- */

static void wm_ping(void *, xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const xdg_wm_base_listener wm_listener = {wm_ping};

static void window_configure(void *data, xdg_surface *window, uint32_t serial) {
	auto *s = static_cast<Core::State *>(data);
	xdg_surface_ack_configure(window, serial);
	s->configured = true;
	s->dirty = true;
}
static const xdg_surface_listener window_listener = {window_configure};

static void toplevel_configure(void *data, xdg_toplevel *, int32_t width, int32_t height,
		wl_array *) {
	auto *s = static_cast<Core::State *>(data);
	if (width > 0 && height > 0 && (width != s->width || height != s->height)) {
		s->width = width;
		s->height = height;
		s->resized = true;
	}
}
static void toplevel_close(void *data, xdg_toplevel *) {
	static_cast<Core::State *>(data)->bridge.emit({Event::Closed});
}
static void toplevel_bounds(void *, xdg_toplevel *, int32_t, int32_t) {}
static void toplevel_capabilities(void *, xdg_toplevel *, wl_array *) {}
static const xdg_toplevel_listener toplevel_listener = {
	toplevel_configure, toplevel_close, toplevel_bounds, toplevel_capabilities,
};

static void output_geometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
	const char *, const char *, int32_t) {}
static void output_mode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
static void output_done(void *, wl_output *) {}
static void output_scale(void *data, wl_output *, int32_t factor) {
	auto *s = static_cast<Core::State *>(data);
	if (factor != s->scale) {
		s->scale = factor;
		s->resized = true;
	}
}
static const wl_output_listener output_listener = {
	output_geometry, output_mode, output_done, output_scale,
};

static void touch_down(void *data, wl_touch *, uint32_t, uint32_t, wl_surface *, int32_t id,
		wl_fixed_t fx, wl_fixed_t fy) {
	auto *s = static_cast<Core::State *>(data);
	if (id < 0 || id >= max_touch) {
		return;
	}
	float x = wl_fixed_to_double(fx), y = wl_fixed_to_double(fy);
	NodeId node = s->tree.hit(x, y);
	s->touches[id] = {true, node, x, y, x, y};
	if (node) {
		s->bridge.emit({Event::Pressed, node, x, y});
	}
}
static void touch_up(void *data, wl_touch *, uint32_t, uint32_t, int32_t id) {
	auto *s = static_cast<Core::State *>(data);
	if (id < 0 || id >= max_touch || !s->touches[id].down) {
		return;
	}
	auto &t = s->touches[id];
	t.down = false;
	if (!t.node) {
		return;
	}
	s->bridge.emit({Event::Released, t.node, t.x, t.y});
	if (std::hypot(t.x - t.start_x, t.y - t.start_y) < tap_slop && s->tree.hit(t.x, t.y) == t.node) {
		s->bridge.emit({Event::Clicked, t.node, t.x, t.y});
	}
}
static void touch_motion(void *data, wl_touch *, uint32_t, int32_t id, wl_fixed_t fx,
		wl_fixed_t fy) {
	auto *s = static_cast<Core::State *>(data);
	if (id >= 0 && id < max_touch && s->touches[id].down) {
		s->touches[id].x = wl_fixed_to_double(fx);
		s->touches[id].y = wl_fixed_to_double(fy);
	}
}
static void touch_frame(void *, wl_touch *) {}
static void touch_cancel(void *data, wl_touch *) {
	auto *s = static_cast<Core::State *>(data);
	for (auto &t : s->touches) {
		t.down = false;
	}
}
static const wl_touch_listener touch_listener = {
	touch_down, touch_up, touch_motion, touch_frame, touch_cancel,
};

static void seat_capabilities(void *data, wl_seat *seat, uint32_t caps) {
	auto *s = static_cast<Core::State *>(data);
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !s->touch) {
		s->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(s->touch, &touch_listener, s);
	}
}
static void seat_name(void *, wl_seat *, const char *) {}
static const wl_seat_listener seat_listener = {seat_capabilities, seat_name};

static void registry_global(void *data, wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	auto *s = static_cast<Core::State *>(data);
	if (!strcmp(interface, wl_compositor_interface.name)) {
		s->compositor = static_cast<wl_compositor *>(
			wl_registry_bind(registry, name, &wl_compositor_interface, 4));
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		s->wm_base = static_cast<xdg_wm_base *>(
			wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
		xdg_wm_base_add_listener(s->wm_base, &wm_listener, s);
	} else if (!strcmp(interface, wl_seat_interface.name) && !s->seat) {
		s->seat = static_cast<wl_seat *>(
			wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
		wl_seat_add_listener(s->seat, &seat_listener, s);
	} else if (!strcmp(interface, wl_output_interface.name) && !s->output) {
		s->output = static_cast<wl_output *>(
			wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
		wl_output_add_listener(s->output, &output_listener, s);
	}
}
static void registry_remove(void *, wl_registry *, uint32_t) {}
static const wl_registry_listener registry_listener = {registry_global, registry_remove};

/* ---- setup ---- */

bool Core::State::connect() {
	display = wl_display_connect(nullptr);
	if (!display) {
		fprintf(stderr, "starry-ui: cannot connect to the compositor\n");
		return false;
	}
	wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, this);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display); // output scale, seat capabilities
	if (!compositor || !wm_base) {
		fprintf(stderr, "starry-ui: compositor lacks xdg-shell\n");
		return false;
	}
	surface = wl_compositor_create_surface(compositor);
	window = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(window, &window_listener, this);
	toplevel = xdg_surface_get_toplevel(window);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, this);
	xdg_toplevel_set_title(toplevel, options.title.c_str());
	xdg_toplevel_set_app_id(toplevel, options.app_id.c_str());
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) >= 0) {
	}
	resized = false;
	return configured;
}

bool Core::State::create_gl() {
	egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));
	if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, nullptr, nullptr)) {
		return false;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
	};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &count) || count < 1) {
		return false;
	}
	const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs);
	wl_surface_set_buffer_scale(surface, scale);
	egl_window = wl_egl_window_create(surface, width * scale, height * scale);
	egl_surface = eglCreateWindowSurface(egl_display, config,
		reinterpret_cast<EGLNativeWindowType>(egl_window), nullptr);
	if (egl_context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE ||
			!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
		return false;
	}
	eglSwapInterval(egl_display, 1); // swaps follow the compositor's frame callbacks
	return renderer.init(options.regular_font.c_str(), options.bold_font.c_str());
}

/* ---- drawing: each node's scale is about its own centre ---- */

void Core::State::draw(NodeId id, float alpha, float k, float tx, float ty) {
	Node *node = tree.get(id);
	if (!node) {
		return;
	}
	alpha *= node->opacity.value;
	if (alpha <= 0.002f) {
		return;
	}
	float s = node->scale.value;
	float cx = node->x + node->width / 2, cy = node->y + node->height / 2;
	tx += k * (cx * (1 - s) + node->tx.value);
	ty += k * (cy * (1 - s) + node->ty.value);
	k *= s;

	float x = node->x * k + tx, y = node->y * k + ty;
	Color background = node->background.value;
	background.a *= alpha;
	renderer.rect(x, y, node->width * k, node->height * k, node->radius * k, background);
	if (node->kind == Kind::Label) {
		Color color = node->text_color;
		color.a *= alpha;
		renderer.text(x, y, node->text, node->font_size * k, node->bold, color);
	}
	for (NodeId child : node->children) {
		draw(child, alpha, k, tx, ty);
	}
}

void Core::State::frame() {
	if (resized) {
		wl_surface_set_buffer_scale(surface, scale);
		wl_egl_window_resize(egl_window, width * scale, height * scale, 0, 0);
		resized = false;
	}
	renderer.begin(width * scale, height * scale, scale);
	tree.layout(width, height, renderer);
	draw(tree.root(), 1, 1, 0, 0);
	tree.settle();
	eglSwapBuffers(egl_display, egl_surface);
}

/* ---- the render thread ---- */

void Core::State::run() {
	bool animating = false;
	while (running) {
		auto mutations = bridge.take_mutations();
		double now = now_seconds();
		for (const auto &mutation : mutations) {
			tree.apply(mutation, now);
		}
		dirty |= !mutations.empty();
		animating = tree.step(now);
		if ((dirty || animating) && configured) {
			frame(); // blocks until the compositor wants the next frame
			dirty = false;
		}

		while (wl_display_prepare_read(display) != 0) {
			wl_display_dispatch_pending(display);
		}
		wl_display_flush(display);
		pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {wake_fd, POLLIN, 0}};
		int timeout = animating || dirty ? 0 : -1;
		if (poll(fds, 2, timeout) < 0 && errno != EINTR) {
			wl_display_cancel_read(display);
			break;
		}
		if (fds[0].revents & POLLIN) {
			wl_display_read_events(display);
		} else {
			wl_display_cancel_read(display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) {
			bridge.emit({Event::Closed});
			break;
		}
		if (fds[1].revents & POLLIN) {
			uint64_t count;
			ssize_t ignored = read(wake_fd, &count, sizeof(count));
			(void)ignored;
		}
		wl_display_dispatch_pending(display);
	}
}

void Core::State::teardown() {
	if (egl_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (egl_surface != EGL_NO_SURFACE) {
			eglDestroySurface(egl_display, egl_surface);
		}
		if (egl_context != EGL_NO_CONTEXT) {
			eglDestroyContext(egl_display, egl_context);
		}
		eglTerminate(egl_display);
	}
	if (egl_window) {
		wl_egl_window_destroy(egl_window);
	}
	if (display) {
		wl_display_disconnect(display);
	}
	if (wake_fd >= 0) {
		close(wake_fd);
	}
}

/* ---- Core ---- */

Core::Core(Bridge &bridge, CoreOptions options) : bridge_(bridge), options_(std::move(options)) {}

Core::~Core() {
	stop();
}

bool Core::start() {
	state_ = std::make_unique<State>(bridge_, options_);
	state_->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	int fd = state_->wake_fd;
	bridge_.wake_core = [fd] {
		uint64_t one = 1;
		ssize_t ignored = write(fd, &one, sizeof(one));
		(void)ignored;
	};
	std::promise<bool> ready;
	auto started = ready.get_future();
	thread_ = std::thread([this, &ready] {
		State &s = *state_;
		bool ok = s.connect() && s.create_gl();
		ready.set_value(ok);
		if (ok) {
			s.run();
		}
		s.teardown();
	});
	if (!started.get()) {
		thread_.join();
		return false;
	}
	return true;
}

void Core::stop() {
	if (thread_.joinable()) {
		state_->running = false;
		if (bridge_.wake_core) {
			bridge_.wake_core();
		}
		thread_.join();
	}
}

} // namespace sui
