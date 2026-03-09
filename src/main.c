#define GLAD_GL_IMPLEMENTATION
#include <glad2/gl.h>

#define SOKOL_IMPL
#define SOKOL_GLCORE
#include <sokol/sokol_app.h>

#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg/nanovg.h>
#include <nanovg/nanovg_gl.h>

#define PFH_IMPLEMENTATION
#include <perfect-freehand-c/perfect_freehand.h>

#include "ds.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

typedef struct { float x, y; } vec2;
typedef struct { float x, y, z; } vec3;
typedef struct { float r, g, b, a; } color;

struct stroke {
	vec2 point;
};

DA_DEFINE(vec2, vec2_list)
DA_DEFINE(int, int_list)

struct object {
	struct int_list  *starts;
	struct vec2_list *points;
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
static const float point_min_dist = 1.0f;
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
		printf("	Starts (%llu): ", obj->starts->len);
		for (j = 0; j < obj->starts->len; j++)
 			printf("%d ", obj->starts->elems[j]);
		puts("");

		printf("	Points (%llu)\n", obj->points->len);
		// for (j = 0; j < obj->points->len; j++)
 		// 	printf("(%f, %f) ", obj->points->elems[j].x, obj->points->elems[j].y);
		// puts("");
	}
}

void drawing_continue_stroke(vec2 input)
{
	struct object *obj = objects->elems + objects->len - 1;
	vec2 dist;

	if (obj->points->len != 0) {
		dist.x = input.x - obj->points->elems[obj->points->len-1].x;		
		dist.y = input.y - obj->points->elems[obj->points->len-1].y;		

		if (dist.x*dist.x + dist.y*dist.y < point_min_dist*point_min_dist)
			return;
	}
	obj->points = vec2_list_append(obj->points, input);
}
void drawing_start_stroke(vec2 input)
{
	struct object *obj = objects->elems + objects->len - 1;

	drawing_continue_stroke(input);
	obj->starts = int_list_append(obj->starts, obj->points->len-1);
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
			drawing_continue_stroke(screen_to_world( (vec2) { e->mouse_x, e->mouse_y } ));
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
				objects = object_list_append(objects, (struct object){
					.points = vec2_list_create(DA_INITIAL_CAPACITY),
					.starts = int_list_create(DA_INITIAL_CAPACITY)
				});
			}
			is_drawing_stroke = true;
			drawing_start_stroke(screen_to_world( (vec2) { e->mouse_x, e->mouse_y } ));
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

		for (j = k = 0; j < obj->points->len; j++) {
			if (j != obj->starts->elems[k]) {
				nvgLineTo(vg, obj->points->elems[j].x, -obj->points->elems[j].y);
				continue;
			}
			if (j != 0)
				nvgStroke(vg);

			nvgBeginPath(vg);
			nvgStrokeWidth(vg, 3);
			nvgLineCap(vg, NVG_ROUND);
			nvgLineJoin(vg, NVG_ROUND);
			nvgStrokeColor(vg, nvgRGBA(204, 255, 0, 255));
			nvgMoveTo(vg, obj->points->elems[j].x, -obj->points->elems[j].y);
			k++;
		}
		nvgStroke(vg);
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