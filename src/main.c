#define GLAD_GL_IMPLEMENTATION
#include <glad2/gl.h>

#define SOKOL_IMPL
#define SOKOL_GLCORE
#include <sokol/sokol_app.h>

#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg/nanovg.h>
#include <nanovg/nanovg_gl.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

typedef struct { float x, y; } vec2;
typedef struct { float x, y, z; } vec3;
typedef struct { float r, g, b, a; } color;

static float zoomFrac = 0.1f;

static const color CLEAR_COLOR_DEFAULT = { .1f, .1f, .1f, .0f };
static color clear_color = { .1f, .1f, .1f, 1.0f };

static int screen_width, screen_height;
static NVGcontext* vg;
static vec2 mouse_screen;
static vec2 mouse_world;
static vec2 camera = {0, 0};
static float zoom = 1.0f;

static bool is_panning = false;
static vec2 pan_pivot_mouse;
static vec2 pan_pivot_camera;

static bool is_drawing = false;
static const float input_dist_threshold = 1.0f;
static vec2 drawinput[2048];
static int drawinput_len = 0;

static inline vec2 screen_to_world(vec2 screen)
{
	return (vec2){
		.x = ( (screen.x - screen_width/2) / zoom) + camera.x,
		.y = ( (screen_height/2 - screen.y) / zoom) + camera.y,
	};
}

void test()
{
	// (World - OldCamera) * OldZoom = (World - NewCamera) * NewZoom
}

void init(void) 
{
	screen_width = sapp_width();
	screen_height = sapp_height();
	gladLoaderLoadGL();
	vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
}

void cleanup()
{
	gladLoaderUnloadGL();
	nvgDeleteGL3(vg);
}

void record_drawinput(vec2 input)
{
	vec2 dist;

	if (drawinput_len != 0) {
		dist.x = input.x - drawinput[drawinput_len-1].x;		
		dist.y = input.y - drawinput[drawinput_len-1].y;		

		if (dist.x*dist.x + dist.y*dist.y < input_dist_threshold*input_dist_threshold)
			return;
	}
	drawinput[drawinput_len++] = input;
}

void event(const sapp_event *e)
{
	switch(e->type) {
	case SAPP_EVENTTYPE_RESIZED:
		screen_width = sapp_width();
		screen_height = sapp_height();
		break;
	case SAPP_EVENTTYPE_KEY_DOWN:
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
		if (is_drawing && drawinput_len < ARRAY_SIZE(drawinput)) {
			record_drawinput(screen_to_world( (vec2) { e->mouse_x, e->mouse_y } ));
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_DOWN:
		if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
			is_panning = true;
			clear_color = (color) { 1, 1, 1, 1 };
			pan_pivot_mouse = mouse_screen;
			pan_pivot_camera = camera;
		}
		if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && !is_drawing) {
			is_drawing = true;
			clear_color = (color) { .15, .15, .15, 1 };
			drawinput_len = 0;
		}
		break;
	case SAPP_EVENTTYPE_MOUSE_UP:
		is_panning = is_panning && e->mouse_button != SAPP_MOUSEBUTTON_MIDDLE;
		is_drawing = is_drawing && e->mouse_button != SAPP_MOUSEBUTTON_LEFT;
		clear_color = CLEAR_COLOR_DEFAULT;
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

void frame(void) 
{
	int i;
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

		if (is_drawing) {
			nvgBeginPath(vg);
			nvgStrokeWidth(vg, 3);
			nvgLineCap(vg, NVG_ROUND);
			nvgLineJoin(vg, NVG_ROUND);
			nvgStrokeColor(vg, nvgRGBA(204, 255, 0, 255));
				nvgMoveTo(vg, drawinput[0].x, -drawinput[0].y);
				for (i = 1; i < drawinput_len; i++)
					nvgLineTo(vg, drawinput[i].x, -drawinput[i].y);
			nvgStroke(vg);
		}

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