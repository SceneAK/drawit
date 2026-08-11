#define GLAD_GL_IMPLEMENTATION
#include <glad2/gl.h>

#include <sokol/sokol_app.h> 
#include <sokol/sokol_time.h>

#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg/nanovg.h>
#include <nanovg/nanovg_gl.h>

#define PFH_IMPLEMENTATION
#include <perfect-freehand/pfh.h>

#include <math.h>
#include <stdint.h>

#include "types.h"

#define CMD_HIST_MAX 256
#define STROKE_BOUNDS_MARGIN 5
#define VELOCITY_MAX_IN 1200 /* will mult. by screen_dpi_scale (inch) */
#define STATUS_LINE_MAX 512
#define POINT_INIT { .coord = { FLT_MAX, FLT_MAX }, .pressure = .5 }


typedef struct { vec2 coord; float pressure; } point;

#define T point
#define name da_point
#include "da.t.h"

enum theme {
	THEME_DARK,
	THEME_LIGHT,
	THEME_MAX,
};

enum object_kind {
	OBJECT_KIND_GROUP,
	OBJECT_KIND_STROKE,
	OBJECT_KIND_TEXT,
};

struct object_data {
	vec2             pos;
	rect             bounds;
	enum object_kind kind;
};

#define T struct object_data *
#define name da_object_ptr
#include "da.t.h"

struct stroke_desc {
	struct object_data obj;

	size_t        input_idx,
	              input_count;
	size_t        vertex_idx,
	              vertex_count;
	const color  *colors;
	bool          deleted;
};

#define T struct stroke_desc
#define name da_stroke_desc
#include "da.t.h"

struct stroke_ctx {
	struct da_point       *input_da;
	pfh_vec2_buf           pfh_vertex_buf;
	struct da_stroke_desc *desc_da;
};

struct text_obj {
	struct object_data obj;

	int            font_handle;
	float          font_size;
	float          line_height;
	const color   *colors;
	struct gapbuf *buf;
};

#define T struct text_obj
#define name da_text_obj
#include "da.t.h"

struct text_ctx {
	struct da_text_obj *text_da;
};

struct canvas {
	struct stroke_ctx stroke_ctx;
	struct text_ctx text_ctx;
};

enum cmd_type {
	CMD_NONE,
	CMD_STROKE_CREATE,
	CMD_STROKE_DELETE,
};

struct cmd {
	enum cmd_type type;
	union {
		struct cmd_stroke_data {
			int idx;
			struct stroke_ctx *ctx;
			struct da_point *point_da; 
			const color *colors;
		} stroke;
	} v;
};

struct cmd_hist {
	long before_first;
	long last;
	long cursor;
	struct cmd cmds[CMD_HIST_MAX];
};

enum mode {
	MODE_DRAW,
	MODE_COMMAND,
	MODE_TEXT,
	MODE_DRAG,
};


#define T struct cmd
#define name da_cmd
#include "da.t.h"

/************ GLOBALS & STATE ************/

/* CONSTANTS */
static const u8 APP_ICON_32x32[] = {
	#ifndef __INTELLISENSE__
		#include "gen/icon32x32.inc"
	#else
		0
	#endif
};
#include "gen/inconsolata_ttf.h"

static const color COLOR_DEEP_CHARCOAL = COLOR_INIT_HEX(0x121212FF);
static const color COLOR_SKIN = COLOR_INIT_HEX(0xF5E1D2FF);
static const color COLOR_SCENE = COLOR_INIT_HEX(0xCCFF00FF);
static const color COLOR_MARIGOLD = COLOR_INIT_HEX(0xD97706FF);
static const color COLOR_HOTPINK = COLOR_INIT_HEX(0xFF69B4FF);
static const color COLOR_RASPBERRY = COLOR_INIT_HEX(0xD81B60FF);
static const color COLOR_TURQUOISE = COLOR_INIT_HEX(0x40E0D0FF);
static const color COLOR_TEAL = COLOR_INIT_HEX(0x009688FF);

static const color COLORS_BACKGROUND[THEME_MAX] = { COLOR_DEEP_CHARCOAL, COLOR_SKIN };
static const color COLORS_CONTRAST[THEME_MAX] = { COLOR_SKIN, COLOR_DEEP_CHARCOAL };
static const color COLORS_YELLOW[THEME_MAX] = { COLOR_SCENE, COLOR_MARIGOLD };
static const color COLORS_BLUE[THEME_MAX] = { COLOR_TURQUOISE, COLOR_TEAL };
static const color COLORS_RED[THEME_MAX] = { COLOR_HOTPINK, COLOR_RASPBERRY };

/* NVG & SCREEN  */
static float screen_dpi_scale; /* needs to be cached due to sokol wackyness */
static int screen_width, screen_height;

static NVGcontext *vg;
static const color *clear_colors;

static const float FONT_SIZE_DEFAULT = 64.0;
static int font_handle;

/* APP */
static enum mode mode = MODE_DRAW;
static enum theme theme = THEME_DARK;

static char status_line[STATUS_LINE_MAX];
static size_t status_line_len = 0;

/* MODE DRAW  */
static struct canvas canvas;

static bool is_drawing_stroke = false;
static bool is_deleting_stroke = false;
static bool is_selecting_obj = false;

static const color *colors_primary;
static const color *colors_secondary;
static const color **active_colors;

static vec2 mouse_screen;
static vec2 mouse_world;
static bool mouse_in_frame = true;
static point mouse_point;

static const float CAMERA_ZOOM_FRAC = 0.1f;
static vec2 camera_pos = {0, 0};
static float camera_zoom = 1.0f;

static bool is_panning = false;
static vec2 pan_pivot_mouse;
static vec2 pan_pivot_camera;

static const pfh_stroke_opts STROKE_OPTS = {
	.size = 16,
	.thinning = .55,
	.streamline = .45,
	.smoothing = .55,
	.easing = NULL,
	.simulate_pressure = false,
	.is_complete = false,
	.start = {
		.cap = true,
		.taper = PFH_TAPER_NONE,
		.easing = NULL,
	},
	.end = {
		.cap = true,
		.taper = PFH_TAPER_NONE,
		.easing = NULL,
	},
	.last = false,
};

static struct text_obj *text_in_edit;

static struct da_object_ptr *selected_obj_da = NULL;

/* CMD_HIST */
static struct cmd cmd_curr;
static struct cmd_hist cmd_hist;
static long cmd_save_idx = 0;


/************ HELPERS ************/

static inline vec2 screen_to_world(vec2 screen)
{
	return (vec2){
		.x = ( (screen.x - screen_width/2) / camera_zoom) + camera_pos.x,
		.y = ( (screen_height/2 - screen.y) / camera_zoom) + camera_pos.y,
	};
}

static inline NVGcolor color_to_NVGcolor(color c)
{
	return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline void status_line_set(const char *str)
{
	size_t len = snprintf(status_line, ARRAY_SIZE(status_line), "%s", str);
	status_line_len = min(len, ARRAY_SIZE(status_line)-1);
}

struct canvas canvas_create_empty()
{
	struct stroke_ctx stroke_ctx = {
		.desc_da = da_stroke_desc_create(DA_INITIAL_CAPACITY),
		.input_da = da_point_create(DA_INITIAL_CAPACITY),
	};
	struct text_ctx text_ctx = {
		.text_da = da_text_obj_create(DA_INITIAL_CAPACITY),
	};

	pfh_vec2_buf_init(&stroke_ctx.pfh_vertex_buf, DA_INITIAL_CAPACITY);

	return (struct canvas) { stroke_ctx, text_ctx, };
}


/************ CANVAS: STROKE ************/

void stroke_ctx_print(const struct stroke_ctx *ctx) 
{
	size_t i;
	struct stroke_desc *s;

	printf("inputs (%zu)\n", ctx->input_da->count);
	printf("vertices (%zu)\n", ctx->pfh_vertex_buf.count);
	puts("desc_da:");
	for (i = 0; i < ctx->desc_da->count; i++) {
		s = ctx->desc_da->elems + i;
		printf("stroke_desc[%zu]\n", i);
		printf("  input: idx %zu, count %zu\n", s->input_idx, s->input_count);
		printf("  vertex: idx %zu, count %zu\n", s->vertex_idx, s->vertex_count);
		printf("  colors[%d]: (%d, %d, %d, %d)\n", theme, s->colors[theme].r, s->colors[theme].g, s->colors[theme].b, s->colors[theme].a);
	}
	puts("");
}

void stroke_ctx_begin(struct stroke_ctx *ctx, const color *colors)
{
	ctx->desc_da = da_stroke_desc_append(ctx->desc_da, (struct stroke_desc) { 
		.obj.bounds   = BOUNDS_INIT,

		.input_idx    = ctx->input_da->count, 
		.input_count  = 0,
		.vertex_idx   = ctx->pfh_vertex_buf.count, 
		.vertex_count = 0,
		.colors       = colors,
		.deleted      = false,
	});
}

void stroke_ctx_render_last(struct stroke_ctx *ctx)
{
	struct stroke_desc *s = DA_LAST(ctx->desc_da);

	ctx->pfh_vertex_buf.count = s->vertex_idx;
	pfh_get_stroke(
		&ctx->pfh_vertex_buf,
		(pfh_point*)ctx->input_da->elems + s->input_idx,
		s->input_count,
		&STROKE_OPTS
	);
	s->vertex_count = ctx->pfh_vertex_buf.count - s->vertex_idx;
}

void stroke_ctx_append_point(struct stroke_ctx *ctx, point mouse_point)
{
	struct stroke_desc *s = DA_LAST(ctx->desc_da);
	const vec2 PT_EXTENTS = vec2_all(STROKE_OPTS.size/2 + STROKE_BOUNDS_MARGIN);

	ctx->input_da = da_point_append(ctx->input_da, mouse_point);
	s->input_count++;
	s->obj.bounds = rect_fit_rect(s->obj.bounds, rect_create(mouse_point.coord, PT_EXTENTS));

	stroke_ctx_render_last(ctx);
}

void stroke_ctx_append_points(struct stroke_ctx *ctx, const point pts[], int count)
{
	const vec2 PT_EXTENTS = vec2_all(STROKE_OPTS.size/2 + STROKE_BOUNDS_MARGIN);

	struct stroke_desc *s = DA_LAST(ctx->desc_da);
	int i;

	ctx->input_da = da_point_append_n(ctx->input_da, pts, count);
	s->input_count += count;
	for (i = 0; i < count; i++)
		s->obj.bounds = rect_fit_rect(s->obj.bounds, rect_create(pts[i].coord, PT_EXTENTS));

	stroke_ctx_render_last(ctx);
}

void stroke_ctx_mark_delete(struct stroke_ctx *ctx, int stroke_idx, bool deleted)
{
	ctx->desc_da->elems[stroke_idx].deleted = deleted;
}

void stroke_ctx_delete_last(struct stroke_ctx *ctx)
{
	struct stroke_desc *s = DA_LAST(ctx->desc_da);

	ctx->input_da->count = s->input_idx;
	ctx->pfh_vertex_buf.count = s->vertex_idx;
	ctx->desc_da->count--;
}

float stroke_ctx_dist(const struct stroke_ctx *ctx, int stroke_idx, vec2 v)
{
	const struct stroke_desc *s = ctx->desc_da->elems + stroke_idx;
	const point *s_inputs = ctx->input_da->elems + s->input_idx;
	float closest_dist2 = FLT_MAX;
	float dist2;
	size_t i;

	for (i = 0; i < s->input_count; i++) {
		dist2 = vec2_dist2(v, s_inputs[i].coord);
		if (dist2 < closest_dist2)
			closest_dist2 = dist2;
	}
	return closest_dist2;
}

int stroke_ctx_closest(const struct stroke_ctx *ctx, vec2 v)
{
	size_t closest_stroke_idx = -1;
	float closest_dist2 = FLT_MAX;
	float dist2;
	size_t i;

	for (i = 0; i < ctx->desc_da->count; i++) {
		if (ctx->desc_da->elems[i].deleted)
			continue;
		if (!rect_contains(ctx->desc_da->elems[i].obj.bounds, v))
			continue;
		dist2 = stroke_ctx_dist(ctx, i, v);
		if (dist2 >= closest_dist2)
			continue;
		closest_dist2 = dist2;
		closest_stroke_idx = i;
	}
	return closest_stroke_idx;
}

/************ CANVAS: TEXT ************/

void text_ctx_print(const struct text_ctx *ctx)
{
	size_t i;
	struct text_obj *t;

	puts("text_da:");
	for (i = 0; i < ctx->text_da->count; i++) {
		t = ctx->text_da->elems + i;
		printf("text[%zu]\n", i);
		printf("   pos: (%f, %f)\n"
		       "  font: %d\n"
		       "  size: %f\n"
		       "gapbuf: s(%zu) e(%zu) c(%zu)\n",
			t->obj.pos.x, t->obj.pos.y,
			t->font_handle,
			t->font_size,
			t->buf->gap_start, t->buf->gap_end, t->buf->capacity
		);
	}
	puts("");
	
}

struct text_obj text_obj_create(const color *colors, float font_size, vec2 pos)
{
	const float LEADING_RATIO = 1.2f;

	return (struct text_obj) {
		.obj.pos = pos,
		.obj.bounds = BOUNDS_INIT,

		.font_handle = font_handle,
		.font_size = font_size,
		.line_height = font_size * LEADING_RATIO,
		.colors = colors,
		.buf = gapbuf_create(GAPBUF_INITIAL_ALLOC),
	};
}

static inline void text_ctx_append(struct text_ctx *ctx, const color *colors, float font_size, vec2 pos)
{
	ctx->text_da = da_text_obj_append(ctx->text_da, text_obj_create(colors, font_size, pos));
}

void text_ctx_edit(struct text_ctx *ctx, point mouse_point);
void text_ctx_delete(struct text_ctx *ctx, int text_idx, bool deleted);
float text_ctx_dist(const struct text_ctx *ctx, int text_idx, vec2 v);
int text_ctx_closest_idx(const struct text_ctx *ctx, vec2 v);

/************ CMD ************/

void cmd_forget(struct cmd *cmd)
{
	switch (cmd->type) {
	case CMD_STROKE_DELETE:
		break;
	case CMD_STROKE_CREATE:
		free(cmd->v.stroke.point_da);
		break;
	default: break;
	}
	cmd->type = CMD_NONE;
}

void cmd_undo(struct cmd c)
{
	switch (c.type) {
	case CMD_STROKE_DELETE:
		stroke_ctx_mark_delete(c.v.stroke.ctx, c.v.stroke.idx, false);
		break;
	case CMD_STROKE_CREATE:
		stroke_ctx_delete_last(c.v.stroke.ctx);
		break;
	default: break;
	}
}

void cmd_redo(struct cmd c)
{
	switch (c.type) {
	case CMD_STROKE_DELETE:
		stroke_ctx_mark_delete(
			c.v.stroke.ctx,
			c.v.stroke.idx,
			true
		);
		break;
	case CMD_STROKE_CREATE:
		stroke_ctx_begin(c.v.stroke.ctx, c.v.stroke.colors);
		stroke_ctx_append_points(
			c.v.stroke.ctx,
			c.v.stroke.point_da->elems,
			c.v.stroke.point_da->count
		);
		break;
	default: break;
	}
}


/************ CMD_HIST ************/

void cmd_hist_record(struct cmd_hist *hist, struct cmd cmd)
{
	hist->cursor = RINGBUF_INCR(hist->cursor, ARRAY_SIZE(hist->cmds), 1);

	cmd_forget(hist->cmds + hist->cursor);
	hist->cmds[hist->cursor] = cmd;

	hist->last = hist->cursor;

	if (hist->before_first == hist->last)
		hist->before_first = RINGBUF_INCR(hist->before_first, ARRAY_SIZE(hist->cmds), 1);
}

void cmd_hist_undo(struct cmd_hist *hist)
{
	if (hist->cursor == hist->before_first) {
		status_line_set("Already at oldest history");
		return;
	}

	cmd_undo(hist->cmds[hist->cursor]);
	hist->cursor = RINGBUF_DECR(hist->cursor, ARRAY_SIZE(hist->cmds), 1);
}

static void cmd_hist_redo(struct cmd_hist *hist)
{
	if (hist->cursor == hist->last) {
		status_line_set("Already at newest change");
		return;
	}

	hist->cursor = RINGBUF_INCR(hist->cursor, ARRAY_SIZE(hist->cmds), 1);
	cmd_redo(hist->cmds[hist->cursor]);
}


/************ EVENT MODE HANDLING  ************/

void mode_switch_drawing(void)
{
	mode = MODE_DRAW;
	sapp_show_mouse(false);
}

void mode_switch_command(void)
{
	mode = MODE_COMMAND;
	status_line_set(":");
	sapp_show_mouse(true);
}

void mode_switch_text(void)
{
	mode = MODE_TEXT;
	sapp_show_mouse(true);
}

void mode_switch_drag(void)
{
	mode = MODE_DRAG;
	sapp_show_mouse(false);
}

/* MODE_COMMAND */

void command_exec(const char *str)
{
	if (str[0] == ':') {
		str++;
		if (drawit_strcasecmp(str, "light") == 0 || (str[0] == 'l')) {
			theme = THEME_LIGHT;
			status_line_set("theme=light");
		} else if (drawit_strcasecmp(str, "dark") == 0 || (str[0] == 'd')) {
			theme = THEME_DARK;
			status_line_set("theme=dark");
		} else if (drawit_strcasecmp(str, "quit") == 0 || (str[0] == 'q')) {
			sapp_request_quit();
		} else if (drawit_strcasecmp(str, "print") == 0 || (str[0] == 'p')) {
			stroke_ctx_print(&canvas.stroke_ctx);
			status_line_set("debug print stroke ctx to stdout");
		} else {
			status_line_set("unknown command");
		}
	}
}

void command_mode_event(const sapp_event *e)
{
	if (e->type == SAPP_EVENTTYPE_CHAR && status_line_len < STATUS_LINE_MAX-1) {
		status_line[status_line_len++] = e->char_code;
		return;
	}
	if (e->type != SAPP_EVENTTYPE_KEY_DOWN)
		return;

	switch (e->key_code) {
	case SAPP_KEYCODE_BACKSPACE:
		if (status_line_len <= 1)
			break;

		status_line_len--;
		break;
	case SAPP_KEYCODE_ENTER:
		status_line[status_line_len] = '\0';
		command_exec(status_line); /* status line is reused to write results. don't zero */

		mode_switch_drawing();
		break;
	case SAPP_KEYCODE_ESCAPE:
		mode_switch_drawing();
		status_line_len = 0;
		break;
	default: break;
	}
}

/* MODE_TEXT */

void text_mode_event(const sapp_event *e)
{
	struct text_obj *t = text_in_edit;

	if (!t) return;

	if (e->type == SAPP_EVENTTYPE_CHAR)
		gapbuf_insert(&t->buf, (unsigned char)e->char_code);

	if (e->type != SAPP_EVENTTYPE_KEY_DOWN)
		return;

	switch (e->key_code) {
	case SAPP_KEYCODE_LEFT:
		gapbuf_open(t->buf, t->buf->gap_start-1);
		break;
	case SAPP_KEYCODE_RIGHT:
		gapbuf_open(t->buf, t->buf->gap_start+1);
		break;
	case SAPP_KEYCODE_UP:
		break;
	case SAPP_KEYCODE_DOWN:
		break;
	case SAPP_KEYCODE_BACKSPACE:
		gapbuf_delete(t->buf);
		break;
	case SAPP_KEYCODE_ENTER:
		gapbuf_insert(&t->buf, (unsigned char)'\n');
		break;
	case SAPP_KEYCODE_ESCAPE:
		mode_switch_drawing();
		break;
	default: break;
	}
}

/* MODE_DRAG */

void drag_mode_event(const sapp_event *e)
{
	if (e->type != SAPP_EVENTTYPE_KEY_DOWN)
		return;

	switch (e->key_code) {
	case SAPP_KEYCODE_ESCAPE:
		mode_switch_drawing();
		break;
	default: break;
	}
}

/* MODE_DRAW */

void draw_mode_event_mouse(const sapp_event *e)
{
	static u64 last_move = 0;
	double delta, vel;

	static point last_pt = POINT_INIT;

	switch (e->type) {
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		mouse_screen.x = e->mouse_x;
		mouse_screen.y = e->mouse_y;
		mouse_world = screen_to_world(mouse_screen);
		/* fall through */
	case SAPP_EVENTTYPE_MOUSE_DOWN:
	case SAPP_EVENTTYPE_MOUSE_UP:
		delta = stm_sec(stm_laptime(&last_move));

		mouse_point.coord = mouse_world;
		vel = ( sqrtf(vec2_dist2(mouse_point.coord, last_pt.coord)) / delta );

		mouse_point.pressure = 1 - min(vel / (VELOCITY_MAX_IN*screen_dpi_scale), 1);
		mouse_point.pressure = last_pt.pressure * .75 + mouse_point.pressure * .25; /* blend */

		last_pt = mouse_point;
		break;
	default: break;
	}
}

void draw_mode_event_camera(const sapp_event *e)
{
	switch(e->type) {
	case SAPP_EVENTTYPE_MOUSE_ENTER:
		mouse_in_frame = true;
		break;
	case SAPP_EVENTTYPE_MOUSE_LEAVE:
		mouse_in_frame = false;
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		mouse_screen.x = e->mouse_x;
		mouse_screen.y = e->mouse_y;
		mouse_world = screen_to_world(mouse_screen);

		if (is_panning) {
			camera_pos.x = pan_pivot_camera.x + (pan_pivot_mouse.x - mouse_screen.x)/camera_zoom;
			camera_pos.y = pan_pivot_camera.y + (mouse_screen.y - pan_pivot_mouse.y)/camera_zoom;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_UP:
		is_panning = is_panning && e->mouse_button != SAPP_MOUSEBUTTON_MIDDLE;
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
			is_panning = true;
			pan_pivot_mouse = mouse_screen;
			pan_pivot_camera = camera_pos;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_SCROLL:
		float ratio = (1 + CAMERA_ZOOM_FRAC * e->scroll_y);
		camera_zoom *= ratio;
		
		/* (World - OldCamera) * OldZoom = (World - NewCamera) * NewZoom */
		camera_pos.x = mouse_world.x - (mouse_world.x - camera_pos.x) / ratio;
		camera_pos.y = mouse_world.y - (mouse_world.y - camera_pos.y) / ratio;
		break;
	default: break;
	}
}

void draw_mode_stroke_begin(struct stroke_ctx *ctx, point pt, const color *colors, struct cmd *cmd)
{
	stroke_ctx_begin(ctx, colors);
	stroke_ctx_append_point(ctx, pt);


	if (cmd->type != CMD_NONE)
		puts("Warning: something ain't right during stroke begin. cmd->is not NONE.");
	cmd->type = CMD_STROKE_CREATE;

	cmd->v.stroke.ctx = ctx;
	cmd->v.stroke.idx = ctx->desc_da->count-1;
	cmd->v.stroke.colors = colors;
	cmd->v.stroke.point_da = da_point_create(DA_INITIAL_CAPACITY);

	cmd->v.stroke.point_da = da_point_append(cmd->v.stroke.point_da, pt);
}

void draw_mode_stroke_try_append(struct stroke_ctx *ctx, point pt, struct cmd *cmd)
{
	const int MIN_PX = 2;
	static point last_valid_pt = POINT_INIT;

	if (vec2_dist2(pt.coord, last_valid_pt.coord) < MIN_PX*MIN_PX)
		return;
	last_valid_pt = pt;

	if (cmd->type != CMD_STROKE_CREATE)
		puts("Warning: something ain't right during stroke try append. cmd_curr is not STROKE_CREATE.");
	stroke_ctx_append_point(ctx, pt);
	cmd->v.stroke.point_da = da_point_append(cmd->v.stroke.point_da, pt);
}

void draw_mode_stroke_end(struct cmd_hist *hist, struct cmd *cmd)
{
	cmd_hist_record(hist, cmd_curr);
	cmd->type = CMD_NONE;
}

void draw_mode_stroke_mark_delete(struct stroke_ctx *ctx, int idx, struct cmd_hist *hist, struct cmd cmd)
{
	stroke_ctx_mark_delete(
		ctx, 
		idx,
		true
	);

	if (cmd.type != CMD_NONE)
		puts("Warning: Something ain't right during stroke mark delete. cmd_curr is not NONE.");

	cmd.type = CMD_STROKE_DELETE;
	cmd.v.stroke.ctx = ctx;
	cmd.v.stroke.idx = idx;

	cmd_hist_record(hist, cmd);
	cmd.type = CMD_NONE;
}

void draw_mode_event(const sapp_event *e)
{
	int idx;

	draw_mode_event_mouse(e);
	draw_mode_event_camera(e);

	switch (e->type) {
	case SAPP_EVENTTYPE_CHAR: 
		if (e->char_code == ':') {
			mode_switch_command();
			break;
		}
		if (e->char_code == 'i') {
			text_ctx_append(&canvas.text_ctx, *active_colors, FONT_SIZE_DEFAULT, mouse_world);
			text_in_edit = DA_LAST(canvas.text_ctx.text_da);
			mode_switch_text();
			return;
		}
		break;
	default: break;
	}

	switch (e->type) {
	/* Mouse */
	case SAPP_EVENTTYPE_MOUSE_UP:
		if (is_drawing_stroke) {
			is_drawing_stroke = false;
			draw_mode_stroke_end(&cmd_hist, &cmd_curr);
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (status_line_len)
			status_line_len = 0;

		if (e->modifiers & SAPP_MODIFIER_ALT) {
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
				
			}
		}

		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
			active_colors = &colors_primary;
		else if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
			active_colors = &colors_secondary;

		if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT || e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
			if (is_drawing_stroke)
				draw_mode_stroke_end(&cmd_hist, &cmd_curr);

			is_drawing_stroke = true;
			draw_mode_stroke_begin(&canvas.stroke_ctx, mouse_point, *active_colors, &cmd_curr);
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		if (is_drawing_stroke) {
			draw_mode_stroke_try_append(&canvas.stroke_ctx, mouse_point, &cmd_curr);
		}
		break;

	/* Keyboard */
	case SAPP_EVENTTYPE_KEY_DOWN:
		if (status_line_len)
			status_line_len = 0;

		active_colors = &colors_primary;
		if (e->modifiers & SAPP_MODIFIER_ALT)
			active_colors = &colors_secondary;

		switch (e->key_code) {
		case SAPP_KEYCODE_1:
			*active_colors = COLORS_YELLOW;
			break;
		case SAPP_KEYCODE_2:
			*active_colors = COLORS_RED;
			break;
		case SAPP_KEYCODE_3:
			*active_colors = COLORS_BLUE;
			break;
		case SAPP_KEYCODE_4:
			*active_colors = COLORS_CONTRAST;
			break;

		case SAPP_KEYCODE_Z:
			if (e->modifiers & SAPP_MODIFIER_CTRL)
				cmd_hist_undo(&cmd_hist);
			break;
		case SAPP_KEYCODE_R:
			if (e->modifiers & SAPP_MODIFIER_CTRL)
				cmd_hist_redo(&cmd_hist);
			break;

		case SAPP_KEYCODE_X:
			is_deleting_stroke = true;
			break;

		case SAPP_KEYCODE_LEFT_SHIFT:
			is_selecting_obj = true;
			break;

		case SAPP_KEYCODE_M:
			cmd_save_idx = cmd_hist.cursor;
			break;
		case SAPP_KEYCODE_APOSTROPHE:
			/* TODO: This is dangerous. Figure out forward or backwards, else infinte loop */
			while (cmd_hist.cursor != cmd_save_idx) {
				cmd_hist_undo(&cmd_hist); 
			}
			break;
		case SAPP_KEYCODE_EQUAL:
			while (cmd_hist.cursor != cmd_hist.last) {
				cmd_hist_redo(&cmd_hist);
			}
			break;

		default: break;
		}

		active_colors = &colors_primary;
		break;
	case SAPP_EVENTTYPE_KEY_UP:
		is_selecting_obj = false;
		if (e->key_code == SAPP_KEYCODE_LEFT_SHIFT) {
			mode_switch_drag();
		}

		if (e->key_code == SAPP_KEYCODE_X) {
			draw_mode_stroke_end(&cmd_hist, &cmd_curr); /* no weird shenanigans mid draw */

			idx = stroke_ctx_closest(&canvas.stroke_ctx, mouse_world);
			if (idx >= 0)
				draw_mode_stroke_mark_delete(&canvas.stroke_ctx, idx, &cmd_hist, cmd_curr);
		}
		break;
	default: break;
	}
}


/************ NVG DRAW FUNCTIONS ************/

static inline void nvg_fontsize_ctx(NVGcontext *ctx, const struct text_obj *txt)
{
	nvgFontSize(ctx, txt->font_size);
	nvgFontFaceId(ctx, txt->font_handle);
	nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

static inline vec2 get_cursor_offset(const struct text_obj *txt)
{
	size_t start, i;
	float x = 0, 
	      y = txt->line_height/2;


	start = 0;
	for (i = 0; i < txt->buf->gap_start; i++) {
		if (txt->buf->data[i] == '\n') {
			y += txt->line_height;
			start = i+1;
		}
	}
	nvg_fontsize_ctx(vg, txt);
	x = nvgTextBounds(vg, 0, 0, txt->buf->data + start, txt->buf->data + i, NULL);
	return (vec2) { x, y };
}

static inline vec2 get_cursor_size(const struct text_obj *txt)
{
	float ascender, descender;
	float advance;

	nvg_fontsize_ctx(vg, txt);
	nvgTextMetrics(vg, &ascender, &descender, NULL);

	if (txt->buf->gap_end == txt->buf->capacity)
		advance = nvgTextBounds(vg, 0, 0, " ", NULL, NULL);
	else
		advance = nvgTextBounds(vg, 0, 0, 
			txt->buf->data + txt->buf->gap_end,
			txt->buf->data + txt->buf->gap_end + 1, /* Assumes ASCII */
			NULL
		);

	return (vec2) { advance, ascender - descender };
}

void draw_text_cursor_overlay(const struct text_obj *txt, bool local)
{
	vec2 offset = get_cursor_offset(txt);
	vec2 size = get_cursor_size(txt);
	vec2 pos = {
		(local ? 0 : txt->obj.pos.x) + offset.x,
		(local ? 0 : txt->obj.pos.y) + offset.y,
	};
	const char *c = txt->buf->gap_end < txt->buf->capacity ? txt->buf->data + txt->buf->gap_end : " ";

	nvgFillColor(vg, color_to_NVGcolor(COLORS_CONTRAST[theme]));
	nvgBeginPath(vg);
	nvgRect(vg, pos.x, pos.y - size.y/2, size.x, size.y); /* offset y to middle */
	nvgFill(vg);

	nvg_fontsize_ctx(vg, txt);
	nvgFillColor(vg, color_to_NVGcolor(COLORS_BACKGROUND[theme]));
	nvgText(vg, pos.x, pos.y, c, c+1); /* I give up messing with GlobalCompositeOperations */
}

void draw_text(const struct text_obj *txt, bool draw_cursor)
{
	const struct gapbuf *buf = txt->buf;

	size_t start, i;
	float x = 0,
	      y = 0;

	nvg_fontsize_ctx(vg, txt);
	nvgFillColor(vg, color_to_NVGcolor(txt->colors[theme]));

	nvgSave(vg);
	nvgScale(vg, 1, -1);
	nvgTranslate(vg, txt->obj.pos.x, -txt->obj.pos.y);
		x = 0;
		y = txt->line_height/2;

		start = i = 0;
		for (;;) {
			if (i >= buf->capacity) { /* NOTE: nvg already checks start != end */
				nvgText(vg, x, y, buf->data + start, buf->data + i);
				break;
			}

			if (i == buf->gap_start && (buf->gap_start != buf->gap_end)) {
				x = nvgText(vg, x, y, buf->data + start, buf->data + i);
				start = i = buf->gap_end;
				continue;
			}

			if (buf->data[i] == '\n') {
				nvgText(vg, x, y, buf->data + start, buf->data + i);
				start = i+1;

				y += txt->line_height;
				x = 0;
			}
			i++;
		}
		if (draw_cursor)
			draw_text_cursor_overlay(txt, true);
	nvgRestore(vg);
}

void draw_text_ctx(const struct text_ctx *ctx)
{
	size_t i;

	for (i = 0; i < ctx->text_da->count; i++) {
		draw_text(
			ctx->text_da->elems + i, 
			mode == MODE_TEXT 
			&& ctx->text_da->elems + i == text_in_edit 
		);
	}
}

void draw_rect(rect r)
{
	nvgBeginPath(vg);
		nvgRect(vg, r.x0, r.y0, r.x1-r.x0, r.y1-r.y0);
	nvgStrokeColor(vg, nvgRGBA(0, 255, 0, 255));
	nvgStrokeWidth(vg, 2.0f);
	nvgStroke(vg);
}

void draw_stroke_ctx(struct stroke_ctx *ctx)
{
	size_t i, j;
	struct stroke_desc *s;
	pfh_vec2 *s_vertices;
	pfh_vec2 p0, p1;
	int tmp;

	for (i = 0; i < ctx->desc_da->count; i++) {
		s = ctx->desc_da->elems + i;
		if (s->deleted)
			continue;
		if (s->vertex_count < 3) /* shouldn't happen, a dot is 13 segs */
			continue;

		s_vertices = ctx->pfh_vertex_buf.elems + s->vertex_idx;

		nvgBeginPath(vg);
		nvgFillColor(vg, color_to_NVGcolor(s->colors[theme]));

		p0 = s_vertices[s->vertex_count-1];
		p1 = s_vertices[0];
		nvgMoveTo(vg, (p0.x + p1.x) / 2, (p0.y + p1.y) / 2);

		for (j = 0; j < s->vertex_count; j++) {
			p0 = s_vertices[j];
			p1 = (j+1) == s->vertex_count
				? s_vertices[0]
				: s_vertices[j + 1];
			nvgQuadTo(vg, p0.x, p0.y, (p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
		}
		nvgFill(vg);
	}
	if (is_deleting_stroke) {
		tmp = stroke_ctx_closest(ctx, mouse_world);
		if (tmp == -1)
			return;
		draw_rect(ctx->desc_da->elems[tmp].obj.bounds);
	}
}

void draw_status_line(void)
{
	const float FONT_SIZE = 26.0;
	const vec2 coord = { 0 + FONT_SIZE, screen_height - FONT_SIZE };

	if (!status_line_len)
		return;

	nvgFontSize(vg, FONT_SIZE);
	nvgFontFaceId(vg, font_handle);
	nvgFillColor(vg, color_to_NVGcolor(COLORS_CONTRAST[theme]));
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgText(vg, coord.x, coord.y, status_line, status_line + status_line_len);
}


/************ SOKOL APP ************/

void init(void) 
{
	stm_setup();

	canvas = canvas_create_empty();
	selected_obj_da = da_object_ptr_create(DA_INITIAL_CAPACITY);
	selected_obj_da = da_object_ptr_append(selected_obj_da, NULL);

	clear_colors = COLORS_BACKGROUND;
	colors_primary = COLORS_YELLOW;
	colors_secondary = COLORS_RED;
	active_colors = &colors_primary;

	screen_width = sapp_width();
	screen_height = sapp_height();
	gladLoaderLoadGL();
	vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);

	font_handle = nvgCreateFontMem(vg, "Inconsolata-Regular-Sub", (u8 *)inconsolata_ttf, inconsolata_ttf_len, 0);

	mode_switch_drawing();

#ifdef _WIN32
	#include <windows.h>
	HWND hwnd = (HWND)sapp_win32_get_hwnd();
	ShowWindow(hwnd, SW_MAXIMIZE);
#endif
}

void cleanup(void)
{
	gladLoaderUnloadGL();
	nvgDeleteGL3(vg);
}

void event(const sapp_event *e)
{
	switch (e->type) {
	case SAPP_EVENTTYPE_RESIZED:
		screen_width = sapp_width();
		screen_height = sapp_height();
		break;
	default: break;
	}

	switch (mode) {
	case MODE_DRAW:
		draw_mode_event(e);
		break;
	case MODE_TEXT:
		text_mode_event(e);
		break;
	case MODE_DRAG:
		drag_mode_event(e);
		break;
	case MODE_COMMAND:
		command_mode_event(e);
		break;
	default: break;
	}
}

void frame(void) 
{
	color c;
	screen_dpi_scale = sapp_dpi_scale();

	glViewport(0, 0, screen_width, screen_height);
	glClearColor(clear_colors[theme].r/255.0f, clear_colors[theme].g/255.0f, clear_colors[theme].b/255.0f, clear_colors[theme].a/255.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	nvgBeginFrame(vg, screen_width/screen_dpi_scale, screen_height/screen_dpi_scale, screen_dpi_scale);
		nvgSave(vg);
		nvgTranslate(vg, screen_width/2, screen_height/2);
		nvgScale(vg, camera_zoom, -camera_zoom);
		nvgTranslate(vg, -camera_pos.x, -camera_pos.y);
			draw_stroke_ctx(&canvas.stroke_ctx);
			draw_text_ctx(&canvas.text_ctx);
		nvgRestore(vg);
		if (mode == MODE_DRAW && mouse_in_frame) {
			nvgBeginPath(vg);
				nvgCircle(vg, roundf(mouse_screen.x), round(mouse_screen.y), camera_zoom*STROKE_OPTS.size/1.5);
			c = (*active_colors)[theme];
			c.a /= 1.5;
			nvgFillColor(vg, color_to_NVGcolor(c));
			nvgFill(vg);
		}
		draw_status_line();
	nvgEndFrame(vg);
}

sapp_desc sokol_main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	return (sapp_desc){
		.init_cb      = init,
		.frame_cb     = frame,
		.event_cb     = event,
		.cleanup_cb   = cleanup,
		.window_title = "Drawit",
		.icon         = {
			.sokol_default = false,
			.images = {{
				.width  = 32,
				.height = 32,
				.pixels = SAPP_RANGE(APP_ICON_32x32),
			}}
		},
		.swap_interval = 1,
	};
}

