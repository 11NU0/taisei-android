#include "touch_controls.h"

#ifdef __ANDROID__

#include "config.h"
#include "events.h"
#include "log.h"
#include "memory/memory.h"
#include "renderer/api.h"
#include "resource/font.h"
#include "resource/sprite.h"
#include "video.h"

#include <SDL3/SDL.h>
#include <math.h>

/*
 * Touch controls modeled after browser-based Touhou ports
 * (touhouonline.com / Eagler Touhou), with both movement modes:
 *
 *   - Direct touch ("touch trail = player trail"): touch anywhere and
 *     drag; the player follows your finger. Finger delta is accumulated
 *     and consumed at the keyboard movement speed.
 *   - Virtual joystick: a fixed stick base on the left; drag its knob
 *     to steer. The stick deflection is converted to direction keys.
 *
 * A mode button toggles between the two movement modes.
 *
 * Focus (slow movement): hold the focus button, or press a second
 * finger while dragging (direct touch mode).
 */

#define TC_MAX_FINGERS 16

typedef enum TCButton {
	TC_BTN_SHOT,
	TC_BTN_BOMB,
	TC_BTN_FOCUS,
	TC_BTN_SKIP,
	TC_BTN_AUTO,
	TC_BTN_PAUSE,
	TC_BTN_MODE,
	TC_NUM_BUTTONS,
} TCButton;

typedef struct TCButtonDef {
	float x;
	float y;
	float radius;
	const char *label;
} TCButtonDef;

static const TCButtonDef tc_buttons[TC_NUM_BUTTONS] = {
	[TC_BTN_SHOT]  = { 688, 452, 58, "A" },
	[TC_BTN_BOMB]  = { 566, 508, 46, "B" },
	[TC_BTN_FOCUS] = { 688, 320, 40, "F" },
	[TC_BTN_SKIP]  = { 566, 392, 32, "S" },
	[TC_BTN_AUTO]  = { 688, 188, 30, "∞" },
	[TC_BTN_PAUSE] = { 748,  52, 30, "II" },
	[TC_BTN_MODE]  = { 748, 130, 26, "M" },
};

typedef enum TCMovementMode {
	TC_MODE_DRAG,   // direct touch: drag anywhere to move
	TC_MODE_STICK,  // virtual joystick on the left
} TCMovementMode;

/* virtual stick visual geometry (fixed base position) */
#define TC_STICK_X          140.0f
#define TC_STICK_Y          450.0f
#define TC_STICK_RADIUS     120.0f
#define TC_STICK_KNOB_RADIUS 42.0f
/* deflection beyond this is a full-tilt direction */
#define TC_STICK_MAX_TRAVEL (TC_STICK_RADIUS - TC_STICK_KNOB_RADIUS)
/* below this, no direction is held (deadzone) */
#define TC_STICK_DEADZONE   12.0f
/* direction-key engage angle threshold (cos of 8-way sector half-angle) */
#define TC_STICK_DIR_COS    0.3826834f

/* direct-touch: virtual pixels consumed per frame while a direction is held */
#define TC_MOVE_PER_FRAME 7.0f
/* hysteresis thresholds for the direct-touch direction keys */
#define TC_MOVE_ON_THRESHOLD  2.0f
#define TC_MOVE_OFF_THRESHOLD 0.4f

typedef struct TCFinger {
	SDL_FingerID id;
	bool used;
	bool is_button;
	int button;
	bool is_focus;
} TCFinger;

static struct {
	bool initialized;
	TCFinger fingers[TC_MAX_FINGERS];
	bool button_state[TC_NUM_BUTTONS];
	bool dir_state[4];
	bool auto_fire;
	TCMovementMode move_mode;

	struct {
		bool active;
		SDL_FingerID id;
		float acc_x;
		float acc_y;
		float last_x;
		float last_y;
		float anchor_x;
		float anchor_y;
		float cur_x;
		float cur_y;
	} drag;

	struct {
		bool active;
		SDL_FingerID id;
		float cur_x;
		float cur_y;
	} stick;

	SDL_FingerID focus_id;
} tc;

static const int tc_dir_config[4] = {
	CONFIG_KEY_UP,
	CONFIG_KEY_DOWN,
	CONFIG_KEY_LEFT,
	CONFIG_KEY_RIGHT,
};

static SDL_Scancode tc_button_scancode(int button) {
	switch(button) {
		case TC_BTN_SHOT:  return config_get_int(CONFIG_KEY_SHOT);
		case TC_BTN_BOMB:  return config_get_int(CONFIG_KEY_BOMB);
		case TC_BTN_FOCUS: return config_get_int(CONFIG_KEY_FOCUS);
		case TC_BTN_SKIP:  return config_get_int(CONFIG_KEY_SKIP);
		case TC_BTN_PAUSE: return SDL_SCANCODE_ESCAPE;
		default:           return SDL_SCANCODE_UNKNOWN;
	}
}

static bool tc_scancodes_down[SDL_SCANCODE_COUNT];

bool touch_controls_key_pressed(int scancode) {
	return scancode >= 0 && scancode < SDL_SCANCODE_COUNT && tc_scancodes_down[scancode];
}

static void tc_push_key(SDL_Scancode scancode, bool down) {
	if(scancode == SDL_SCANCODE_UNKNOWN) {
		return;
	}

	tc_scancodes_down[scancode] = down;

	SDL_Event e = {};
	e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	e.key.type = e.type;
	e.key.scancode = scancode;
	e.key.down = down;
	e.key.repeat = false;
	e.key.windowID = SDL_GetWindowID(video_get_window());
	events_defer(&e);
}

static void tc_set_button(int button, bool down) {
	if(tc.button_state[button] == down) {
		return;
	}

	tc.button_state[button] = down;
	tc_push_key(tc_button_scancode(button), down);

	// keep auto-fire alive when the manual shot button is released
	if(button == TC_BTN_SHOT && !down && tc.auto_fire) {
		tc_push_key(tc_button_scancode(TC_BTN_SHOT), true);
	}
}

static void tc_set_dir(int dir, bool down) {
	if(tc.dir_state[dir] == down) {
		return;
	}

	tc.dir_state[dir] = down;
	tc_push_key(config_get_int(tc_dir_config[dir]), down);
}

static void tc_set_auto_fire(bool on) {
	if(tc.auto_fire == on) {
		return;
	}

	tc.auto_fire = on;

	// hold/release the shot key, but don't fight with the manual shot button
	if(on && !tc.button_state[TC_BTN_SHOT]) {
		tc_push_key(tc_button_scancode(TC_BTN_SHOT), true);
	} else if(!on && !tc.button_state[TC_BTN_SHOT]) {
		tc_push_key(tc_button_scancode(TC_BTN_SHOT), false);
	}
}

static void tc_clear_dirs(void) {
	for(int i = 0; i < 4; ++i) {
		tc_set_dir(i, false);
	}
}

static void tc_release_all(void) {
	for(int i = 0; i < TC_NUM_BUTTONS; ++i) {
		tc_set_button(i, false);
	}

	tc_clear_dirs();
	tc_set_auto_fire(false);
	SDL_zero(tc_scancodes_down);

	tc.drag.active = false;
	tc.drag.id = 0;
	tc.drag.acc_x = 0;
	tc.drag.acc_y = 0;
	tc.stick.active = false;
	tc.stick.id = 0;
	tc.focus_id = 0;

	for(int i = 0; i < TC_MAX_FINGERS; ++i) {
		tc.fingers[i].used = false;
	}
}

static void tc_finger_to_virtual(const SDL_TouchFingerEvent *f, float *vx, float *vy) {
	SDL_Window *window = video_get_window();
	int pw = SCREEN_W;
	int ph = SCREEN_H;
	SDL_GetWindowSizeInPixels(window, &pw, &ph);

	FloatRect vp;
	video_get_viewport(&vp);

	float px = f->x * pw;
	float py = f->y * ph;

	*vx = (px - vp.x) * SCREEN_W / vp.w;
	*vy = (py - vp.y) * SCREEN_H / vp.h;
}

static TCFinger *tc_finger_slot(SDL_FingerID id, bool create) {
	TCFinger *free_slot = NULL;

	for(int i = 0; i < TC_MAX_FINGERS; ++i) {
		TCFinger *f = &tc.fingers[i];

		if(f->used && f->id == id) {
			return f;
		}

		if(!f->used && free_slot == NULL) {
			free_slot = f;
		}
	}

	if(create && free_slot) {
		*free_slot = (TCFinger) { .id = id, .used = true, .button = -1 };
		return free_slot;
	}

	return NULL;
}

static int tc_hit_button(float x, float y) {
	for(int i = 0; i < TC_NUM_BUTTONS; ++i) {
		const TCButtonDef *b = &tc_buttons[i];
		float dx = x - b->x;
		float dy = y - b->y;
		float r = b->radius * 1.25f;

		if(dx * dx + dy * dy <= r * r) {
			return i;
		}
	}

	return -1;
}

static bool tc_hit_stick_base(float x, float y) {
	float dx = x - TC_STICK_X;
	float dy = y - TC_STICK_Y;
	return dx * dx + dy * dy <= TC_STICK_RADIUS * TC_STICK_RADIUS;
}

static void tc_toggle_mode(void) {
	tc.move_mode = tc.move_mode == TC_MODE_DRAG ? TC_MODE_STICK : TC_MODE_DRAG;

	// reset movement state on mode switch
	tc.drag.active = false;
	tc.drag.id = 0;
	tc.drag.acc_x = 0;
	tc.drag.acc_y = 0;
	tc.stick.active = false;
	tc.stick.id = 0;
	tc_clear_dirs();

	log_info("Touch movement mode: %s",
		tc.move_mode == TC_MODE_DRAG ? "direct touch" : "virtual joystick");
}

static bool tc_handle_finger_down(SDL_TouchFingerEvent *event) {
	float x, y;
	tc_finger_to_virtual(event, &x, &y);

	TCFinger *f = tc_finger_slot(event->fingerID, true);

	if(f == NULL) {
		return false;
	}

	int button = tc_hit_button(x, y);

	if(button >= 0) {
		f->is_button = true;
		f->button = button;

		if(button == TC_BTN_AUTO) {
			f->button = -1; // not a hold button
			tc_set_auto_fire(!tc.auto_fire);
		} else if(button == TC_BTN_MODE) {
			f->button = -1; // not a hold button
			tc_toggle_mode();
		} else {
			tc_set_button(button, true);
		}

		return false;
	}

	if(tc.move_mode == TC_MODE_STICK) {
		// joystick mode: grab the stick when touching its base
		if(!tc.stick.active && tc_hit_stick_base(x, y)) {
			tc.stick.active = true;
			tc.stick.id = event->fingerID;
			tc.stick.cur_x = x;
			tc.stick.cur_y = y;
		}
		return false;
	}

	// direct touch mode
	if(!tc.drag.active) {
		tc.drag.active = true;
		tc.drag.id = event->fingerID;
		tc.drag.acc_x = 0;
		tc.drag.acc_y = 0;
		tc.drag.last_x = x;
		tc.drag.last_y = y;
		tc.drag.anchor_x = x;
		tc.drag.anchor_y = y;
		tc.drag.cur_x = x;
		tc.drag.cur_y = y;
	} else if(!tc.focus_id) {
		// second finger anywhere = focus (slow movement)
		f->is_focus = true;
		tc.focus_id = event->fingerID;
		tc_set_button(TC_BTN_FOCUS, true);
	}

	return false;
}

static bool tc_handle_finger_motion(SDL_TouchFingerEvent *event) {
	TCFinger *f = tc_finger_slot(event->fingerID, false);

	if(f == NULL) {
		return false;
	}

	float x, y;
	tc_finger_to_virtual(event, &x, &y);

	if(tc.drag.active && tc.drag.id == event->fingerID) {
		tc.drag.acc_x += x - tc.drag.last_x;
		tc.drag.acc_y += y - tc.drag.last_y;
		tc.drag.last_x = x;
		tc.drag.last_y = y;
		tc.drag.cur_x = x;
		tc.drag.cur_y = y;
	} else if(tc.stick.active && tc.stick.id == event->fingerID) {
		tc.stick.cur_x = x;
		tc.stick.cur_y = y;
	} else if(f->is_button && f->button >= 0) {
		int hit = tc_hit_button(x, y);
		tc_set_button(f->button, hit == f->button);
	}

	return false;
}

static bool tc_handle_finger_up(SDL_TouchFingerEvent *event) {
	TCFinger *f = tc_finger_slot(event->fingerID, false);

	if(f == NULL) {
		return false;
	}

	if(tc.drag.active && tc.drag.id == event->fingerID) {
		tc.drag.active = false;
		tc.drag.id = 0;
		tc.drag.acc_x = 0;
		tc.drag.acc_y = 0;
		tc_clear_dirs();
	} else if(tc.stick.active && tc.stick.id == event->fingerID) {
		tc.stick.active = false;
		tc.stick.id = 0;
		tc_clear_dirs();
	} else if(f->is_focus) {
		tc.focus_id = 0;
		tc_set_button(TC_BTN_FOCUS, false);
	} else if(f->is_button && f->button >= 0) {
		tc_set_button(f->button, false);
	}

	f->used = false;
	return false;
}

static void tc_update_drag(void) {
	if(!tc.drag.active) {
		return;
	}

	// consume accumulated movement credit at the keyboard movement speed,
	// with hysteresis so a direction stays held across frames.
	bool right = tc.dir_state[3]
		? tc.drag.acc_x > TC_MOVE_OFF_THRESHOLD
		: tc.drag.acc_x > TC_MOVE_ON_THRESHOLD;
	bool left = tc.dir_state[2]
		? tc.drag.acc_x < -TC_MOVE_OFF_THRESHOLD
		: tc.drag.acc_x < -TC_MOVE_ON_THRESHOLD;
	bool down = tc.dir_state[1]
		? tc.drag.acc_y > TC_MOVE_OFF_THRESHOLD
		: tc.drag.acc_y > TC_MOVE_ON_THRESHOLD;
	bool up = tc.dir_state[0]
		? tc.drag.acc_y < -TC_MOVE_OFF_THRESHOLD
		: tc.drag.acc_y < -TC_MOVE_ON_THRESHOLD;

	tc_set_dir(3, right);
	tc_set_dir(2, left);
	tc_set_dir(1, down);
	tc_set_dir(0, up);

	if(right) {
		tc.drag.acc_x -= TC_MOVE_PER_FRAME;
	} else if(left) {
		tc.drag.acc_x += TC_MOVE_PER_FRAME;
	} else {
		tc.drag.acc_x *= 0.5f;
	}

	if(down) {
		tc.drag.acc_y -= TC_MOVE_PER_FRAME;
	} else if(up) {
		tc.drag.acc_y += TC_MOVE_PER_FRAME;
	} else {
		tc.drag.acc_y *= 0.5f;
	}
}

static void tc_update_stick(void) {
	if(!tc.stick.active) {
		return;
	}

	float dx = tc.stick.cur_x - TC_STICK_X;
	float dy = tc.stick.cur_y - TC_STICK_Y;
	float dist = hypotf(dx, dy);

	if(dist < TC_STICK_DEADZONE) {
		tc_clear_dirs();
		return;
	}

	// convert stick deflection to 8-way direction keys
	float nx = dx / dist;
	float ny = dy / dist;

	tc_set_dir(3, nx >  TC_STICK_DIR_COS); // right
	tc_set_dir(2, nx < -TC_STICK_DIR_COS); // left
	tc_set_dir(1, ny >  TC_STICK_DIR_COS); // down
	tc_set_dir(0, ny < -TC_STICK_DIR_COS); // up
}

static bool tc_event_handler(SDL_Event *event, void *arg) {
	switch(event->type) {
		case SDL_EVENT_FINGER_DOWN:
			return tc_handle_finger_down(&event->tfinger);

		case SDL_EVENT_FINGER_MOTION:
			return tc_handle_finger_motion(&event->tfinger);

		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_CANCELED:
			return tc_handle_finger_up(&event->tfinger);

		case SDL_EVENT_DID_ENTER_BACKGROUND:
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			tc_release_all();
			return false;

		default:
			return false;
	}
}

void touch_controls_init(void) {
	if(tc.initialized) {
		return;
	}

	tc.move_mode = TC_MODE_DRAG;

	events_register_handler(&(EventHandler) {
		.proc = tc_event_handler,
		.priority = EPRIO_CAPTURE,
		.event_type = 0,
	});

	tc.initialized = true;
	log_info("Virtual touch controls initialized");
}

void touch_controls_shutdown(void) {
	if(!tc.initialized) {
		return;
	}

	tc_release_all();
	events_unregister_handler(tc_event_handler);
	tc.initialized = false;
}

static void tc_draw_box(float cx, float cy, float size, Color color) {
	r_color(color);
	r_mat_mv_push();
	r_mat_mv_translate(cx, cy, 0);
	r_mat_mv_scale(size, size, 1);
	r_draw_quad();
	r_mat_mv_pop();
}

static void tc_draw_frame(float cx, float cy, float size, float thickness, Color color) {
	float h = size * 0.5f;

	r_color(color);

	const float coords[4][4] = {
		{ cx, cy - h + thickness * 0.5f, size, thickness },
		{ cx, cy + h - thickness * 0.5f, size, thickness },
		{ cx - h + thickness * 0.5f, cy, thickness, size },
		{ cx + h - thickness * 0.5f, cy, thickness, size },
	};

	for(int i = 0; i < 4; ++i) {
		r_mat_mv_push();
		r_mat_mv_translate(coords[i][0], coords[i][1], 0);
		r_mat_mv_scale(coords[i][2], coords[i][3], 1);
		r_draw_quad();
		r_mat_mv_pop();
	}
}

static Sprite tc_stick_dummy;

static Sprite *tc_stick_dummy_sprite(void) {
	if(tc_stick_dummy.tex == NULL) {
		Sprite *star = res_sprite("star");

		if(star == NULL) {
			return NULL;
		}

		tc_stick_dummy.tex = star->tex;
		tc_stick_dummy.w = 1;
		tc_stick_dummy.h = 1;
	}

	return &tc_stick_dummy;
}

static void tc_draw_circle(float cx, float cy, float radius, Color inner, Color outer) {
	Sprite *dummy = tc_stick_dummy_sprite();

	if(dummy == NULL) {
		return;
	}

	r_shader("sprite_filled_circle");
	r_uniform_vec4("color_inner", inner.r, inner.g, inner.b, inner.a);
	r_uniform_vec4("color_outer", outer.r, outer.g, outer.b, outer.a);
	r_draw_sprite(&(SpriteParams) {
		.sprite = dummy,
		.pos = { cx, cy },
		.scale = { radius * 2, radius * 2 },
		.blend = BLEND_PREMUL_ALPHA,
		.color = RGBA_MUL_ALPHA(1, 1, 1, 1),
	});
}

static void tc_draw_stick_visual(void) {
	float base_x, base_y, cur_x, cur_y;

	if(tc.move_mode == TC_MODE_STICK) {
		// fixed joystick base on the left
		base_x = TC_STICK_X;
		base_y = TC_STICK_Y;

		if(tc.stick.active) {
			cur_x = tc.stick.cur_x;
			cur_y = tc.stick.cur_y;
		} else {
			cur_x = base_x;
			cur_y = base_y;
		}
	} else {
		// direct touch: stick appears where the drag started
		if(!tc.drag.active) {
			return;
		}

		base_x = tc.drag.anchor_x;
		base_y = tc.drag.anchor_y;
		cur_x = tc.drag.cur_x;
		cur_y = tc.drag.cur_y;
	}

	// clamp the knob inside the base
	float dx = cur_x - base_x;
	float dy = cur_y - base_y;
	float dist = hypotf(dx, dy);
	float kx = base_x;
	float ky = base_y;

	if(dist > TC_STICK_MAX_TRAVEL && dist > 0) {
		kx += dx / dist * TC_STICK_MAX_TRAVEL;
		ky += dy / dist * TC_STICK_MAX_TRAVEL;
	} else {
		kx += dx;
		ky += dy;
	}

	// base ring: transparent center, translucent rim
	tc_draw_circle(base_x, base_y, TC_STICK_RADIUS,
		RGBA(0, 0, 0, 0), RGBA(0.27f, 0.27f, 0.30f, 0.30f));
	r_flush_sprites();

	// knob: solid translucent circle following the finger
	tc_draw_circle(kx, ky, TC_STICK_KNOB_RADIUS,
		RGBA(0.45f, 0.45f, 0.50f, 0.50f), RGBA(0.45f, 0.45f, 0.50f, 0.50f));
}

void touch_controls_draw(void) {
	if(!tc.initialized) {
		return;
	}

	if(tc.move_mode == TC_MODE_DRAG) {
		tc_update_drag();
	} else {
		tc_update_stick();
	}

	Framebuffer *prev_fb = r_framebuffer_current();
	r_framebuffer(NULL);

	r_state_push();
	r_mat_proj_push_ortho(SCREEN_W, SCREEN_H);
	r_mat_mv_push_identity();
	r_shader_standard_notex();
	r_blend(BLEND_PREMUL_ALPHA);
	r_disable(RCAP_DEPTH_TEST);

	Color fill = RGBA(0.15f, 0.15f, 0.2f, 0.10f);
	Color frame = RGBA(0.9f, 0.9f, 1.0f, 0.30f);
	Color active_fill = RGBA(0.8f, 0.6f, 0.2f, 0.25f);

	tc_draw_stick_visual();

	for(int i = 0; i < TC_NUM_BUTTONS; ++i) {
		const TCButtonDef *b = &tc_buttons[i];
		float size = b->radius * 1.5f;
		bool active = tc.button_state[i] || (i == TC_BTN_AUTO && tc.auto_fire);
		tc_draw_box(b->x, b->y, size, active ? active_fill : fill);
		tc_draw_frame(b->x, b->y, size, 2.0f, frame);
	}

	Font *font = res_font_optional("standard");
	ShaderProgram *text_shader = text_get_default_shader();

	// text_get_default_shader() returns NULL before res_post_init(),
	// e.g. when drawing the very first loading screen frame.
	if(font != NULL && text_shader != NULL) {
		r_shader_ptr(text_shader);

		for(int i = 0; i < TC_NUM_BUTTONS; ++i) {
			const TCButtonDef *b = &tc_buttons[i];
			text_draw(b->label, &(TextParams) {
				.font_ptr = font,
				.shader_ptr = text_shader,
				.pos = { b->x, b->y },
				.align = ALIGN_CENTER,
				.color = RGBA(1, 1, 1, 0.75f),
				.blend = BLEND_PREMUL_ALPHA,
			});
		}
	} else {
		log_warn("tc_draw: labels skipped (font=%p shader=%p)",
			(void*)font, (void*)text_shader);
	}

	r_flush_sprites();
	r_mat_mv_pop();
	r_mat_proj_pop();
	r_state_pop();
	r_framebuffer(prev_fb);
}

#endif
