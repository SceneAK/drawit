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
#define FONT_SIZE_DEFAULT 64.0


/********* CORE DATA STRUCTURES **********/
typedef struct { vec2 coord; float pressure; } point;

#define T point
#define name da_point
#include "da.t.h"

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

#define T struct cmd
#define name da_cmd
#include "da.t.h"

/************ APP CTX STRUCTURES *************/
struct camera {
	vec2 pos;
	float zoom;
	float zoom_frac;

	bool is_panning;
	vec2 pan_pivot_mouse;
	vec2 pan_pivot_self;
};

struct mouse {
	vec2 screen;
	vec2 world;
	bool is_outside_frame;
};

struct graphic_ctx {
	float screen_dpi_scale; /* needs to be cached due to sokol wackyness */
	int screen_width, screen_height;

	NVGcontext *vg;
	const color *clear_colors;
	int font_handle;

	struct camera cam;
	struct mouse mouse;
};


enum theme {
	THEME_DARK,
	THEME_LIGHT,
	THEME_MAX,
};

enum mode {
	MODE_DRAW,
	MODE_COMMAND,
	MODE_TEXT,
	MODE_DRAG,
};

struct pen {
	point point_data;
	const color *colors_primary;
	const color *colors_secondary;
	const color **active_colors;
};

struct mode_draw_ctx {
	bool is_drawing_stroke;
	bool is_deleting_stroke;
	bool is_selecting_obj;
};

struct mode_text_ctx {
	struct text_obj *text_in_edit;
};

struct cmd_hist_state {
	struct cmd buf;
	struct cmd_hist hist;
	long save_idx;
};

struct status_line {
	char buf[STATUS_LINE_MAX];
	size_t len;
};

struct drawit_ctx {
	struct graphic_ctx gfx;

	enum theme theme;
	enum mode mode;

	struct status_line status_line;

	struct pen pen;

	struct mode_draw_ctx mode_draw;
	struct mode_text_ctx mode_text;

	struct da_object_ptr *selected_obj_da;

	struct canvas canvas;

	struct cmd_hist_state cmd_hist_state;
};

/************ GLOBALS ************/

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

static const pfh_stroke_opts STROKE_OPTS = {
	.size = 16,
	.thinning = .55,
	.streamline = .45,
	.smoothing = .55,
	.easing = NULL,
	.simulate_pressure = false,
	.is_complete = false,
	.start = { .cap = true, .taper = PFH_TAPER_NONE, .easing = NULL, },
	.end = { .cap = true, .taper = PFH_TAPER_NONE, .easing = NULL, },
	.last = false,
};

static struct drawit_ctx g_drawit = {
	.gfx = { 
		.cam = { .zoom = 1.0f, .zoom_frac = 0.1f }
	}
};

/************ HELPERS ************/

static inline vec2 screen_to_world(const struct graphic_ctx *gfx, vec2 screen)
{
	return (vec2){
		.x = ( (screen.x - gfx->screen_width/2) / gfx->cam.zoom) + gfx->cam.pos.x,
		.y = ( (gfx->screen_height/2 - screen.y) / gfx->cam.zoom) + gfx->cam.pos.y,
	};
}

static inline void set_mouse_world_from_screen(const struct graphic_ctx *gfx, struct mouse *mouse)
{
	mouse->world = screen_to_world(gfx, mouse->screen);
}

static inline NVGcolor color_to_NVGcolor(color c)
{
	return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline void status_line_set(struct status_line *s, const char *str)
{
	s->len = snprintf(s->buf, ARRAY_SIZE(s->buf), "%s", str);
	s->len = min(s->len, ARRAY_SIZE(s->buf)-1);
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
		printf("  colors[%d]: (%d, %d, %d, %d)\n", g_drawit.theme, s->colors[g_drawit.theme].r, s->colors[g_drawit.theme].g, s->colors[g_drawit.theme].b, s->colors[g_drawit.theme].a);
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

void stroke_ctx_append_point(struct stroke_ctx *ctx, point pt)
{
	struct stroke_desc *s = DA_LAST(ctx->desc_da);
	const vec2 PT_EXTENTS = vec2_all(STROKE_OPTS.size/2 + STROKE_BOUNDS_MARGIN);

	ctx->input_da = da_point_append(ctx->input_da, pt);
	s->input_count++;
	s->obj.bounds = rect_fit_rect(s->obj.bounds, rect_create(pt.coord, PT_EXTENTS));

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

struct text_obj text_obj_create(const color *colors, int font_handle, float font_size, vec2 pos)
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

static inline void text_ctx_append(struct text_ctx *ctx, const color *colors, int font_handle, float font_size, vec2 pos)
{
	ctx->text_da = da_text_obj_append(ctx->text_da, text_obj_create(colors, font_handle, font_size, pos));
}

void text_ctx_edit(struct text_ctx *ctx, point pt);
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
		status_line_set(&g_drawit.status_line, "Already at oldest history");
		return;
	}

	cmd_undo(hist->cmds[hist->cursor]);
	hist->cursor = RINGBUF_DECR(hist->cursor, ARRAY_SIZE(hist->cmds), 1);
}

static void cmd_hist_redo(struct cmd_hist *hist)
{
	if (hist->cursor == hist->last) {
		status_line_set(&g_drawit.status_line, "Already at newest change");
		return;
	}

	hist->cursor = RINGBUF_INCR(hist->cursor, ARRAY_SIZE(hist->cmds), 1);
	cmd_redo(hist->cmds[hist->cursor]);
}


/************ EVENT MODE HANDLING  ************/

void mode_switch_drawing(void)
{
	g_drawit.mode = MODE_DRAW;
	sapp_show_mouse(false);
}

void mode_switch_command(void)
{
	g_drawit.mode = MODE_COMMAND;
	status_line_set(&g_drawit.status_line, ":");
	sapp_show_mouse(true);
}

void mode_switch_text(void)
{
	g_drawit.mode = MODE_TEXT;
	sapp_show_mouse(true);
}

void mode_switch_drag(void)
{
	g_drawit.mode = MODE_DRAG;
	sapp_show_mouse(false);
}

/* MODE_COMMAND */

void command_exec(const char *str)
{
	struct canvas *canvas = &g_drawit.canvas;
	struct status_line *status = &g_drawit.status_line;

	if (str[0] == ':') {
		str++;
		if (drawit_strcasecmp(str, "light") == 0 || (str[0] == 'l')) {
			g_drawit.theme = THEME_LIGHT;
			status_line_set(status, "theme=light");
		} else if (drawit_strcasecmp(str, "dark") == 0 || (str[0] == 'd')) {
			g_drawit.theme = THEME_DARK;
			status_line_set(status, "theme=dark");
		} else if (drawit_strcasecmp(str, "quit") == 0 || (str[0] == 'q')) {
			sapp_request_quit();
		} else if (drawit_strcasecmp(str, "print") == 0 || (str[0] == 'p')) {
			stroke_ctx_print(&canvas->stroke_ctx);
			status_line_set(status, "debug print stroke ctx to stdout");
		} else {
			status_line_set(status, "unknown command");
		}
	}
}

void command_mode_event(const sapp_event *e)
{
	struct status_line *status = &g_drawit.status_line;

	if (e->type == SAPP_EVENTTYPE_CHAR && status->len < STATUS_LINE_MAX-1) {
		status->buf[status->len++] = e->char_code;
		return;
	}
	if (e->type != SAPP_EVENTTYPE_KEY_DOWN)
		return;

	switch (e->key_code) {
	case SAPP_KEYCODE_BACKSPACE:
		if (status->len <= 1)
			break;

		status->len--;
		break;
	case SAPP_KEYCODE_ENTER:
		status->buf[status->len] = '\0';
		command_exec(status->buf); /* status line is reused to write results. don't zero */

		mode_switch_drawing();
		break;
	case SAPP_KEYCODE_ESCAPE:
		mode_switch_drawing();
		status->len = 0;
		break;
	default: break;
	}
}

/* MODE_TEXT */

void text_mode_event(const sapp_event *e)
{
	struct mode_text_ctx *ctx = &g_drawit.mode_text;

	struct text_obj *t = ctx->text_in_edit;

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

void draw_mode_event_mouse(struct graphic_ctx *gfx, struct pen *pen, const sapp_event *e)
{
	static u64 last_move = 0;
	double delta, vel;

	static point last_pt = POINT_INIT;

	switch (e->type) {
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		gfx->mouse.screen.x = e->mouse_x;
		gfx->mouse.screen.y = e->mouse_y;
		gfx->mouse.world = screen_to_world(gfx, gfx->mouse.screen);
		/* fall through */
	case SAPP_EVENTTYPE_MOUSE_DOWN:
	case SAPP_EVENTTYPE_MOUSE_UP:
		delta = stm_sec(stm_laptime(&last_move));

		pen->point_data.coord = gfx->mouse.world;
		vel = ( sqrtf(vec2_dist2(pen->point_data.coord, last_pt.coord)) / delta );

		pen->point_data.pressure = 1 - min(vel / (VELOCITY_MAX_IN * gfx->screen_dpi_scale), 1);
		pen->point_data.pressure = last_pt.pressure * .75 + pen->point_data.pressure * .25; /* blend */

		last_pt = pen->point_data;
		break;
	default: break;
	}
}

void draw_mode_event_camera(struct graphic_ctx *gfx, const sapp_event *e)
{
	struct camera *cam = &gfx->cam;
	struct mouse *mouse = &gfx->mouse;

	switch(e->type) {
	case SAPP_EVENTTYPE_MOUSE_ENTER:
		gfx->mouse.is_outside_frame = false;
		break;
	case SAPP_EVENTTYPE_MOUSE_LEAVE:
		gfx->mouse.is_outside_frame = true;
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		mouse->screen.x = e->mouse_x;
		mouse->screen.y = e->mouse_y;
		mouse->world = screen_to_world(gfx, mouse->screen);

		if (cam->is_panning) {
			cam->pos.x = cam->pan_pivot_self.x + (cam->pan_pivot_mouse.x - mouse->screen.x)/cam->zoom;
			cam->pos.y = cam->pan_pivot_self.y + (mouse->screen.y - cam->pan_pivot_mouse.y)/cam->zoom;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_UP:
		cam->is_panning = cam->is_panning && e->mouse_button != SAPP_MOUSEBUTTON_MIDDLE;
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
			cam->is_panning = true;
			cam->pan_pivot_mouse = mouse->screen;
			cam->pan_pivot_self = cam->pos;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_SCROLL:
		float ratio = (1 + cam->zoom_frac * e->scroll_y);
		cam->zoom *= ratio;
		
		/* (World - OldCamera) * OldZoom = (World - NewCamera) * NewZoom */
		cam->pos.x = mouse->world.x - (mouse->world.x - cam->pos.x) / ratio;
		cam->pos.y = mouse->world.y - (mouse->world.y - cam->pos.y) / ratio;
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
		puts("Warning: something ain't right during stroke try append. cmd buffer is not STROKE_CREATE.");
	stroke_ctx_append_point(ctx, pt);
	cmd->v.stroke.point_da = da_point_append(cmd->v.stroke.point_da, pt);
}

void draw_mode_stroke_end(struct cmd_hist_state *state)
{
	cmd_hist_record(&state->hist, state->buf);
	state->buf.type = CMD_NONE;
}

void draw_mode_stroke_mark_delete(struct stroke_ctx *ctx, int idx, struct cmd_hist *hist, struct cmd cmd)
{
	stroke_ctx_mark_delete(
		ctx, 
		idx,
		true
	);

	if (cmd.type != CMD_NONE)
		puts("Warning: Something ain't right during stroke mark delete. cmd buffer is not NONE.");

	cmd.type = CMD_STROKE_DELETE;
	cmd.v.stroke.ctx = ctx;
	cmd.v.stroke.idx = idx;

	cmd_hist_record(hist, cmd);
	cmd.type = CMD_NONE;
}

void draw_mode_event(const sapp_event *e)
{
	struct mode_text_ctx *mode_text = &g_drawit.mode_text;
	struct mode_draw_ctx *mode_draw = &g_drawit.mode_draw;
	struct cmd_hist_state *cmd_hist_state = &g_drawit.cmd_hist_state;
	struct status_line *status = &g_drawit.status_line;
	struct canvas *canvas = &g_drawit.canvas;

	struct graphic_ctx *gfx = &g_drawit.gfx;
	struct pen *pen = &g_drawit.pen;

	int idx;

	draw_mode_event_mouse(gfx, pen, e);
	draw_mode_event_camera(gfx, e);

	switch (e->type) {
	case SAPP_EVENTTYPE_CHAR: 
		if (e->char_code == ':') {
			mode_switch_command();
			break;
		}
		if (e->char_code == 'i') {
			text_ctx_append(&canvas->text_ctx, *pen->active_colors, gfx->font_handle, FONT_SIZE_DEFAULT, gfx->mouse.world);
			mode_text->text_in_edit = DA_LAST(canvas->text_ctx.text_da);
			mode_switch_text();
			return;
		}
		break;
	default: break;
	}

	switch (e->type) {
	/* Mouse */
	case SAPP_EVENTTYPE_MOUSE_UP:
		if (mode_draw->is_drawing_stroke) {
			mode_draw->is_drawing_stroke = false;
			draw_mode_stroke_end(cmd_hist_state);
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (status->len)
			status->len = 0;

		if (e->modifiers & SAPP_MODIFIER_ALT) {
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
				
			}
		}

		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
			pen->active_colors = &pen->colors_primary;
		else if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
			pen->active_colors = &pen->colors_secondary;

		if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT || e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
			if (mode_draw->is_drawing_stroke)
				draw_mode_stroke_end(cmd_hist_state);

			mode_draw->is_drawing_stroke = true;
			draw_mode_stroke_begin(&canvas->stroke_ctx, pen->point_data, *pen->active_colors, &cmd_hist_state->buf);
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		if (mode_draw->is_drawing_stroke) {
			draw_mode_stroke_try_append(&canvas->stroke_ctx, pen->point_data, &cmd_hist_state->buf);
		}
		break;

	/* Keyboard */
	case SAPP_EVENTTYPE_KEY_DOWN:
		if (status->len) status->len = 0;

		pen->active_colors = &pen->colors_primary;
		if (e->modifiers & SAPP_MODIFIER_ALT)
			pen->active_colors = &pen->colors_secondary;

		switch (e->key_code) {
		case SAPP_KEYCODE_1:
			*pen->active_colors = COLORS_YELLOW;
			break;
		case SAPP_KEYCODE_2:
			*pen->active_colors = COLORS_RED;
			break;
		case SAPP_KEYCODE_3:
			*pen->active_colors = COLORS_BLUE;
			break;
		case SAPP_KEYCODE_4:
			*pen->active_colors = COLORS_CONTRAST;
			break;

		case SAPP_KEYCODE_Z:
			if (e->modifiers & SAPP_MODIFIER_CTRL)
				cmd_hist_undo(&cmd_hist_state->hist);
			break;
		case SAPP_KEYCODE_R:
			if (e->modifiers & SAPP_MODIFIER_CTRL)
				cmd_hist_redo(&cmd_hist_state->hist);
			break;

		case SAPP_KEYCODE_X:
			mode_draw->is_deleting_stroke = true;
			break;

		case SAPP_KEYCODE_LEFT_SHIFT:
			mode_draw->is_selecting_obj = true;
			break;

		case SAPP_KEYCODE_M:
			cmd_hist_state->save_idx = cmd_hist_state->hist.cursor;
			break;
		case SAPP_KEYCODE_APOSTROPHE:
			/* TODO: This is dangerous. Figure out forward or backwards, else infinte loop */
			while (cmd_hist_state->hist.cursor != cmd_hist_state->save_idx) {
				cmd_hist_undo(&cmd_hist_state->hist); 
			}
			break;
		case SAPP_KEYCODE_EQUAL:
			while (cmd_hist_state->hist.cursor != cmd_hist_state->hist.last) {
				cmd_hist_redo(&cmd_hist_state->hist);
			}
			break;

		default: break;
		}

		pen->active_colors = &pen->colors_primary;
		break;
	case SAPP_EVENTTYPE_KEY_UP:
		mode_draw->is_selecting_obj = false;
		if (e->key_code == SAPP_KEYCODE_LEFT_SHIFT) {
			mode_switch_drag();
		}

		if (e->key_code == SAPP_KEYCODE_X) {
			draw_mode_stroke_end(cmd_hist_state); /* no weird shenanigans mid draw */

			idx = stroke_ctx_closest(&canvas->stroke_ctx, gfx->mouse.world);
			if (idx >= 0)
				draw_mode_stroke_mark_delete(&canvas->stroke_ctx, idx, &cmd_hist_state->hist, cmd_hist_state->buf);
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

static inline vec2 get_cursor_offset(struct graphic_ctx *gfx, const struct text_obj *txt)
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
	nvg_fontsize_ctx(gfx->vg, txt);
	x = nvgTextBounds(gfx->vg, 0, 0, txt->buf->data + start, txt->buf->data + i, NULL);
	return (vec2) { x, y };
}

static inline vec2 get_cursor_size(struct graphic_ctx *gfx, const struct text_obj *txt)
{
	float ascender, descender;
	float advance;

	nvg_fontsize_ctx(gfx->vg, txt);
	nvgTextMetrics(gfx->vg, &ascender, &descender, NULL);

	if (txt->buf->gap_end == txt->buf->capacity)
		advance = nvgTextBounds(gfx->vg, 0, 0, " ", NULL, NULL);
	else
		advance = nvgTextBounds(gfx->vg, 0, 0, 
			txt->buf->data + txt->buf->gap_end,
			txt->buf->data + txt->buf->gap_end + 1, /* Assumes ASCII */
			NULL
		);

	return (vec2) { advance, ascender - descender };
}

void draw_text_cursor_overlay(struct graphic_ctx *gfx, const struct text_obj *txt, bool local)
{
	vec2 offset = get_cursor_offset(gfx, txt);
	vec2 size = get_cursor_size(gfx, txt);
	vec2 pos = {
		(local ? 0 : txt->obj.pos.x) + offset.x,
		(local ? 0 : txt->obj.pos.y) + offset.y,
	};
	const char *c = txt->buf->gap_end < txt->buf->capacity ? txt->buf->data + txt->buf->gap_end : " ";

	nvgFillColor(gfx->vg, color_to_NVGcolor(COLORS_CONTRAST[g_drawit.theme]));
	nvgBeginPath(gfx->vg);
	nvgRect(gfx->vg, pos.x, pos.y - size.y/2, size.x, size.y); /* offset y to middle */
	nvgFill(gfx->vg);

	nvg_fontsize_ctx(gfx->vg, txt);
	nvgFillColor(gfx->vg, color_to_NVGcolor(COLORS_BACKGROUND[g_drawit.theme]));
	nvgText(gfx->vg, pos.x, pos.y, c, c+1); /* I give up messing with GlobalCompositeOperations */
}

void draw_text(struct graphic_ctx *gfx, const struct text_obj *txt, bool draw_cursor)
{
	const struct gapbuf *buf = txt->buf;

	size_t start, i;
	float x = 0,
	      y = 0;

	nvg_fontsize_ctx(gfx->vg, txt);
	nvgFillColor(gfx->vg, color_to_NVGcolor(txt->colors[g_drawit.theme]));

	nvgSave(gfx->vg);
	nvgScale(gfx->vg, 1, -1);
	nvgTranslate(gfx->vg, txt->obj.pos.x, -txt->obj.pos.y);
		x = 0;
		y = txt->line_height/2;

		start = i = 0;
		for (;;) {
			if (i >= buf->capacity) { /* NOTE: nvg already checks start != end */
				nvgText(gfx->vg, x, y, buf->data + start, buf->data + i);
				break;
			}

			if (i == buf->gap_start && (buf->gap_start != buf->gap_end)) {
				x = nvgText(gfx->vg, x, y, buf->data + start, buf->data + i);
				start = i = buf->gap_end;
				continue;
			}

			if (buf->data[i] == '\n') {
				nvgText(gfx->vg, x, y, buf->data + start, buf->data + i);
				start = i+1;

				y += txt->line_height;
				x = 0;
			}
			i++;
		}
		if (draw_cursor)
			draw_text_cursor_overlay(gfx, txt, true);
	nvgRestore(gfx->vg);
}

void draw_text_ctx(struct graphic_ctx *gfx, const struct text_ctx *ctx, const struct mode_text_ctx *mode_text)
{
	size_t i;

	for (i = 0; i < ctx->text_da->count; i++) {
		draw_text(
			gfx,
			ctx->text_da->elems + i, 
			g_drawit.mode == MODE_TEXT && ctx->text_da->elems + i == mode_text->text_in_edit 
		);
	}
}

void draw_rect(struct graphic_ctx *gfx, rect r)
{
	nvgBeginPath(gfx->vg);
		nvgRect(gfx->vg, r.x0, r.y0, r.x1-r.x0, r.y1-r.y0);
	nvgStrokeColor(gfx->vg, nvgRGBA(0, 255, 0, 255));
	nvgStrokeWidth(gfx->vg, 2.0f);
	nvgStroke(gfx->vg);
}

void draw_stroke_ctx(struct graphic_ctx *gfx, struct stroke_ctx *ctx, const struct mode_draw_ctx *mode_draw)
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

		nvgBeginPath(gfx->vg);
		nvgFillColor(gfx->vg, color_to_NVGcolor(s->colors[g_drawit.theme]));

		p0 = s_vertices[s->vertex_count-1];
		p1 = s_vertices[0];
		nvgMoveTo(gfx->vg, (p0.x + p1.x) / 2, (p0.y + p1.y) / 2);

		for (j = 0; j < s->vertex_count; j++) {
			p0 = s_vertices[j];
			p1 = (j+1) == s->vertex_count
				? s_vertices[0]
				: s_vertices[j + 1];
			nvgQuadTo(gfx->vg, p0.x, p0.y, (p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
		}
		nvgFill(gfx->vg);
	}
	if (mode_draw->is_deleting_stroke) {
		tmp = stroke_ctx_closest(ctx, gfx->mouse.world);
		if (tmp == -1)
			return;
		draw_rect(gfx, ctx->desc_da->elems[tmp].obj.bounds);
	}
}

void draw_status_line(struct graphic_ctx *gfx, const struct status_line *status)
{
	const float FONT_SIZE = 26.0;
	const vec2 coord = { 0 + FONT_SIZE, gfx->screen_height - FONT_SIZE };

	if (!status->len)
		return;

	nvgFontSize(gfx->vg, FONT_SIZE);
	nvgFontFaceId(gfx->vg, gfx->font_handle);
	nvgFillColor(gfx->vg, color_to_NVGcolor(COLORS_CONTRAST[g_drawit.theme]));
	nvgTextAlign(gfx->vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgText(gfx->vg, coord.x, coord.y, status->buf, status->buf + status->len);
}


/************ SOKOL APP ************/

void init(void) 
{
	struct graphic_ctx *gfx = &g_drawit.gfx;
	struct pen *pen = &g_drawit.pen;
	stm_setup();

	g_drawit.canvas = canvas_create_empty();
	g_drawit.selected_obj_da = da_object_ptr_create(DA_INITIAL_CAPACITY);
	g_drawit.selected_obj_da = da_object_ptr_append(g_drawit.selected_obj_da, NULL);

	gfx->clear_colors = COLORS_BACKGROUND;
	pen->colors_primary = COLORS_YELLOW;
	pen->colors_secondary = COLORS_RED;
	pen->active_colors = &pen->colors_primary;

	gfx->screen_width = sapp_width();
	gfx->screen_height = sapp_height();
	gladLoaderLoadGL();
	gfx->vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);

	gfx->font_handle = nvgCreateFontMem(gfx->vg, "Inconsolata-Regular-Sub", (u8 *)inconsolata_ttf, inconsolata_ttf_len, 0);

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
	nvgDeleteGL3(g_drawit.gfx.vg);
}

void event(const sapp_event *e)
{
	struct graphic_ctx *gfx = &g_drawit.gfx;

	switch (e->type) {
	case SAPP_EVENTTYPE_RESIZED:
		gfx->screen_width = sapp_width();
		gfx->screen_height = sapp_height();
		break;
	default: break;
	}

	switch (g_drawit.mode) {
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
	struct graphic_ctx *gfx = &g_drawit.gfx;
	struct mode_text_ctx *mode_text = &g_drawit.mode_text;
	struct mode_draw_ctx *mode_draw = &g_drawit.mode_draw;
	struct pen *pen = &g_drawit.pen;
	struct camera *cam = &gfx->cam;
	struct mouse *mouse = &gfx->mouse;
	struct canvas *canvas = &g_drawit.canvas;

	color c;
	gfx->screen_dpi_scale = sapp_dpi_scale();

	glViewport(0, 0, gfx->screen_width, gfx->screen_height);
	glClearColor(gfx->clear_colors[g_drawit.theme].r/255.0f, gfx->clear_colors[g_drawit.theme].g/255.0f, gfx->clear_colors[g_drawit.theme].b/255.0f, gfx->clear_colors[g_drawit.theme].a/255.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	nvgBeginFrame(gfx->vg, gfx->screen_width/gfx->screen_dpi_scale, gfx->screen_height/gfx->screen_dpi_scale, gfx->screen_dpi_scale);
		nvgSave(gfx->vg);
			nvgTranslate(gfx->vg, gfx->screen_width/2, gfx->screen_height/2);
			nvgScale(gfx->vg, cam->zoom, -cam->zoom);
			nvgTranslate(gfx->vg, -cam->pos.x, -cam->pos.y);

			draw_stroke_ctx(gfx, &canvas->stroke_ctx, mode_draw);
			draw_text_ctx(gfx, &canvas->text_ctx, mode_text);
		nvgRestore(gfx->vg);
		if (g_drawit.mode == MODE_DRAW && !gfx->mouse.is_outside_frame) {
			nvgBeginPath(gfx->vg);
				nvgCircle(gfx->vg, roundf(mouse->screen.x), round(mouse->screen.y), cam->zoom*STROKE_OPTS.size/1.5);
			c = (*pen->active_colors)[g_drawit.theme];
			c.a /= 1.5;
			nvgFillColor(gfx->vg, color_to_NVGcolor(c));
			nvgFill(gfx->vg);
		}
		draw_status_line(gfx, &g_drawit.status_line);
	nvgEndFrame(gfx->vg);
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

