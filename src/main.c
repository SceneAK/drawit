#define GLAD_GL_IMPLEMENTATION
#include <glad2/gl.h>

#define SOKOL_IMPL
#define SOKOL_GLCORE
#include <sokol/sokol_app.h>

#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg/nanovg.h>
#include <nanovg/nanovg_gl.h>

#define PFH_IMPLEMENTATION
#include <perfect-freehand/pfh.h>

#include "ds.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

typedef struct { float x, y; } vec2;
typedef struct { float x, y, z; } vec3;
typedef struct { float r, g, b, a; } color;

typedef struct { vec2 coord; float pressure; } point;

DA_DEFINE(point, point_list)
DA_DEFINE(int, int_list)

struct object {
	struct int_list   *input_starts;
	struct point_list *input_points;
	struct int_list   *stroke_starts;
	pfh_vec2_buff     stroke_buff;
};

DA_DEFINE(struct object, object_list)

static float zoomFrac = 0.1f;

static const color CLEAR_COLOR_DEFAULT = { .1f, .1f, .1f, .0f };
static color clear_color = { .1f, .1f, .1f, 1.0f };

static int screen_width, screen_height;
static NVGcontext *vg;
static vec2 mouse_screen;
static vec2 mouse_world;
static vec2 camera = {0, 0};
static float zoom = 1.0f;

static bool is_panning = false;
static vec2 pan_pivot_mouse;
static vec2 pan_pivot_camera;

static bool is_drawing_obj = false;
static bool is_drawing_stroke = false;
static struct object_list *objects;

static inline vec2 screen_to_world(vec2 screen)
{
	return (vec2){
		.x = ( (screen.x - screen_width/2) / zoom) + camera.x,
		.y = ( (screen_height/2 - screen.y) / zoom) + camera.y,
	};
}

void print_objects(void)
{
	struct object *obj;
	int i, j;

	puts("Objects:");
	for (i = 0; i < objects->len; i++) {
		printf("[%d] Object\n", i);

		obj = objects->elems + i;
		printf("	input_starts (%llu): ", obj->input_starts->len);
		for (j = 0; j < obj->input_starts->len; j++)
 			printf("%d ", obj->input_starts->elems[j]);
		puts("");
		printf("	input_points (%llu)\n", obj->input_points->len);

		printf("	stroke_starts (%llu): ", obj->stroke_starts->len);
		for (j = 0; j < obj->stroke_starts->len; j++)
 			printf("%d ", obj->stroke_starts->elems[j]);
		puts("");

		printf("	stroke_elems (%llu): ", obj->stroke_buff.len);
		for (j = 0; j < obj->stroke_buff.len; j++)
 			printf("(%.1f %.1f) ", obj->stroke_buff.elems[j].x, obj->stroke_buff.elems[j].y);
		puts("");
	}
}

static const pfh_stroke_opts stroke_opts = {
	.size = 8,
	.thinning = .5,
	.streamline = .5,
	.smoothing = .5,
	.easing = NULL,
	.simulate_pressure = true,
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
void draw_stroke_continue(struct object *obj, vec2 input, float pressure)
{
	size_t last_stroke_start_idx = obj->stroke_starts->elems[obj->stroke_starts->len-1];
	size_t last_input_start_idx = obj->input_starts->elems[obj->input_starts->len-1];

	obj->input_points = point_list_append(obj->input_points, (point){ input, pressure });

	// Regenerate the last stroke completely
	obj->stroke_buff.len = last_stroke_start_idx;
	pfh_get_stroke(
		&obj->stroke_buff, 
		(pfh_point*)obj->input_points->elems + last_input_start_idx, 
		obj->input_points->len - last_input_start_idx, 
		&stroke_opts
	);
}
void draw_stroke_start(struct object *obj, vec2 input, float pressure)
{
	obj->input_starts = int_list_append(obj->input_starts, obj->input_points->len);
	obj->stroke_starts = int_list_append(obj->stroke_starts, obj->stroke_buff.len);
	draw_stroke_continue(obj, input, pressure);
}


void init(void) 
{
	objects = object_list_create(DA_INITIAL_CAPACITY);

	screen_width = sapp_width();
	screen_height = sapp_height();
	gladLoaderLoadGL();
	vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
}

void cleanup(void)
{
	gladLoaderUnloadGL();
	nvgDeleteGL3(vg);
}

void event(const sapp_event *e)
{
	switch(e->type) {
	case SAPP_EVENTTYPE_RESIZED:
		screen_width = sapp_width();
		screen_height = sapp_height();
		break;
	case SAPP_EVENTTYPE_KEY_DOWN:
		if (e->key_code == SAPP_KEYCODE_LEFT_CONTROL && is_drawing_obj) {
			is_drawing_obj = false;
			clear_color = CLEAR_COLOR_DEFAULT;
			print_objects();
		}
		break;
	case SAPP_EVENTTYPE_KEY_UP:
		break;
	case SAPP_EVENTTYPE_MOUSE_MOVE:
		mouse_screen.x = e->mouse_x;
		mouse_screen.y = e->mouse_y;
		mouse_world = screen_to_world(mouse_screen);

		if (is_panning) {
			camera.x = pan_pivot_camera.x + (pan_pivot_mouse.x - mouse_screen.x)/zoom;
			camera.y = pan_pivot_camera.y + (mouse_screen.y - pan_pivot_mouse.y)/zoom;
		}
		if (is_drawing_stroke) {
			draw_stroke_continue(
				objects->elems + objects->len-1,
				screen_to_world( (vec2) { e->mouse_x, e->mouse_y } ),
				-1
			);
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
			is_panning = true;
			pan_pivot_mouse = mouse_screen;
			pan_pivot_camera = camera;
		}
		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
			if (!is_drawing_obj) {
				is_drawing_obj = true;
				clear_color = (color) { .13, .2, .13, 1 };
				struct object obj = {
					.input_points  = point_list_create(DA_INITIAL_CAPACITY),
					.input_starts  = int_list_create(DA_INITIAL_CAPACITY),
					.stroke_starts      = int_list_create(DA_INITIAL_CAPACITY),
				};
				pfh_vec2_buff_init(&obj.stroke_buff, DA_INITIAL_CAPACITY);
				objects = object_list_append(objects, obj);
			}
			is_drawing_stroke = true;
			draw_stroke_start(
				objects->elems + objects->len-1,
				screen_to_world( (vec2) { e->mouse_x, e->mouse_y } ),
				-1
			);
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_UP:
		is_panning = is_panning && e->mouse_button != SAPP_MOUSEBUTTON_MIDDLE;
		if (is_drawing_stroke) {
			is_drawing_stroke = false;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_SCROLL:
		float ratio = (1 + zoomFrac * e->scroll_y);
		zoom *= ratio;
		
		// (World - OldCamera) * OldZoom = (World - NewCamera) * NewZoom
		camera.x = mouse_world.x - (mouse_world.x - camera.x) / ratio;
		camera.y = mouse_world.y - (mouse_world.y - camera.y) / ratio;
		break;
	default:
		break;
	}
}

void draw_objects(void)
{
	int i, j, k;
	struct object *obj;

	for (i = 0; i < objects->len; i++) {
		obj = objects->elems + i;

		nvgBeginPath(vg);
		nvgFillColor(vg, nvgRGBA(204, 255, 0, 255));
		for (j = k = 0; j < obj->stroke_buff.len; j++) {
			if (k >= obj->stroke_starts->len || j != obj->stroke_starts->elems[k]) {
				nvgLineTo(vg, obj->stroke_buff.elems[j].x, -obj->stroke_buff.elems[j].y);
				continue;
			}
			nvgMoveTo(vg, obj->stroke_buff.elems[j].x, -obj->stroke_buff.elems[j].y);
			k++;
		}
		nvgFill(vg);
	}

}

void frame(void) 
{
	float dpi = sapp_dpi_scale();

	glViewport(0, 0, screen_width, screen_height);
	glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	nvgBeginFrame(vg, screen_width/dpi, screen_height/dpi, dpi);
	nvgTranslate(vg, screen_width/2, screen_height/2);
	nvgScale(vg, zoom, zoom);
	nvgTranslate(vg, -camera.x, camera.y);

		nvgBeginPath(vg);
			nvgRect(vg, -25, -25, 50, 50);
		nvgFillColor(vg, nvgRGBA(255, 192, 0, 255));
		nvgFill(vg);

		draw_objects();
	nvgEndFrame(vg);
}

sapp_desc sokol_main(int argc, char* argv[]) {
	return (sapp_desc){
		.init_cb      = init,
		.frame_cb     = frame,
		.event_cb     = event,
		.cleanup_cb   = cleanup,
		.window_title = "Drawit",
		
	};
}