#ifndef PFH_H
#define PFH_H

#include <stddef.h>
#include <stdbool.h>

typedef struct { float x, y; } pfh_vec2;
typedef struct { pfh_vec2 coord; float pressure; } pfh_point;
typedef float (*pfh_easing_fn)(float);

typedef struct {
	float size; // 16
	float thinning; // .5
	float streamline; // .5
	float smoothing; // .5
	pfh_easing_fn easing; // Set NULL to default to pfh_ease
	bool simulate_pressure; // true
	bool is_complete; // false
	struct {
		bool cap; // true
		float taper;
		pfh_easing_fn easing; // Set NULL to default to pfh_ease_start/end
	} start, end;
	bool last;
} pfh_stroke_opts;

typedef struct pfh_stroke_point {
	pfh_point point;
	float distance;
	pfh_vec2 vector;
	float running_length;
} pfh_stroke_point;

typedef struct {
	size_t capacity;
	size_t len;
	pfh_vec2 *elems;
} pfh_vec2_buff;

typedef struct {
	size_t capacity;
	size_t len;
	pfh_stroke_point *elems;
} pfh_stroke_point_buff;

void pfh_get_stroke(pfh_vec2_buff *dest, const pfh_point pts[], size_t pts_len, const pfh_stroke_opts *opts);
void pfh_get_stroke_points(pfh_stroke_point_buff *dest, const pfh_point pts[], size_t pts_len, const pfh_stroke_opts *opts);
void pfh_get_stroke_outline_points(pfh_vec2_buff *dest, const pfh_stroke_point stroke_pts[], size_t stroke_pts_len, const pfh_stroke_opts *opts);

#ifdef PFH_IMPLEMENTATION
#include <math.h>
#include <limits.h>
#include <float.h>
#include <string.h>
#include <assert.h>

#define PFH_END_NOISE_THRESHOLD 3
#define PFH_MIN_STREAMLINE_T 0.15
#define PFH_STREAMLINE_T_RANGE 0.85
#define PFH_START_CAP_SEGMENTS 13
#define PFH_END_CAP_SEGMENTS 13
#define PFH_CORNER_CAP_SEGMENTS 13
#define PFH_RATE_OF_PRESSURE_CHANGE 0.275
#define PFH_MIN_RADIUS 0.01

#define PFH_PI 3.141592653589793
#define PFH_FIXED_PI (PFH_PI + 0.0001)

#define PFH_TAPER_NONE 0
#define PFH_TAPER_MAX FLT_MAX

#ifndef pfh_realloc
	#include <stdlib.h>
	#define pfh_realloc(ptr, size) realloc(ptr, size)
#endif

#define PFH_BUFF_GROW_FACTOR 2
#define PFH_BUFF_INIT_CAPACITY 64
#define pfh_max(a, b) ( a > b ? a : b )
#define pfh_buff_reserve(buff, total)                                      \
        do                                                                 \
        {                                                                  \
                if (total >= (buff)->capacity)                             \
                {                                                          \
                        if ((buff)->capacity == 0)                         \
                                (buff)->capacity = PFH_BUFF_INIT_CAPACITY; \
                                                                           \
                        while (total >= (buff)->capacity)                  \
                                (buff)->capacity *= PFH_BUFF_GROW_FACTOR;  \
                        (buff)->elems = pfh_realloc(                       \
                            (buff)->elems,                                 \
                            (buff)->capacity * sizeof(*(buff)->elems));    \
                        assert((buff)->elems && "OOM. buy more ram lol");  \
                }                                                          \
        } while (0)
#define pfh_buff_push_raw(buff, elem) (buff)->elems[(buff)->len++] = (elem);
#define pfh_buff_push(buff, elem)                        \
        do                                               \
        {                                                \
                pfh_buff_reserve(buff, (buff)->len + 1); \
                pfh_buff_push_raw(buff, elem);           \
        } while (0)
#define pfh_buff_left_concat(left, right)                                                                    \
        do                                                                                                   \
        {                                                                                                    \
                pfh_buff_reserve(left, (left)->len + (right)->len);                                          \
                memcpy((left)->elems + (left)->len, (right)->elems, (right)->len * sizeof(*(right)->elems)); \
                (left)->len += (right)->len;                                                                 \
        } while (0)
#define pfh_buff_left_concat_reverse(left, right)                   \
        do                                                          \
        {                                                           \
                pfh_buff_reserve(left, (left)->len + (right)->len); \
                for (size_t i = (right)->len; i-- > 0;)             \
                {                                                   \
                        pfh_buff_push_raw(left, (right)->elems[i]); \
                }                                                   \
        } while (0)

// Placeholder for initial vector & creating 2nd point when only one is provided.
static const pfh_vec2 UNIT_OFFSET = {1, 1};
static pfh_vec2_buff pfh_buff_rightpt = {0};
static pfh_vec2_buff pfh_buff_startcap = {0};
static pfh_vec2_buff pfh_buff_endcap = {0};
static pfh_stroke_point_buff pfh_buff_stroke_pts = {0};


static inline bool pfh_flt_equal(float a, float b)
{
    float diff = fabs(a - b);
    a = fabs(a);
    b = fabs(b);
    return diff <= pfh_max(a, b) * FLT_EPSILON;
}


static inline float pfh_vec2_len(pfh_vec2 a) { return hypotf(a.x, a.y); }
static inline pfh_vec2 pfh_vec2_add(pfh_vec2 a, pfh_vec2 b) { return (pfh_vec2) { a.x + b.x, a.y + b.y }; }
static inline pfh_vec2 pfh_vec2_sub(pfh_vec2 a, pfh_vec2 b) { return (pfh_vec2) { a.x - b.x, a.y - b.y }; }
static inline pfh_vec2 pfh_vec2_mul(pfh_vec2 a, pfh_vec2 b) { return (pfh_vec2) { a.x * b.x, a.y * b.y }; }
static inline pfh_vec2 pfh_vec2_mul_s(pfh_vec2 a, float scale) { return (pfh_vec2) { a.x * scale, a.y * scale }; }
static inline pfh_vec2 pfh_vec2_div_s(pfh_vec2 a, float scale) { return (pfh_vec2) { a.x / scale, a.y / scale }; }
static inline pfh_vec2 pfh_vec2_uni(pfh_vec2 a) { return pfh_vec2_div_s(a, pfh_vec2_len(a)); }
static inline pfh_vec2 pfh_vec2_per(pfh_vec2 a) { return  (pfh_vec2){a.y, -a.x}; }
static inline pfh_vec2 pfh_vec2_neg(pfh_vec2 a) { return  (pfh_vec2){-a.x, -a.y}; }
static inline pfh_vec2 pfh_vec2_prj(pfh_vec2 a, pfh_vec2 b, float c) { return pfh_vec2_add(a, pfh_vec2_mul_s(b, c)); }
static inline bool pfh_vec2_equal(pfh_vec2 a, pfh_vec2 b) { return pfh_flt_equal(a.x, b.x) && pfh_flt_equal(a.y, b.y); }
static inline float pfh_vec2_dot(pfh_vec2 a, pfh_vec2 b) { return a.x * b.x + a.y * b.y; }

static inline float pfh_vec2_dist2(pfh_vec2 from, pfh_vec2 to)
{
	pfh_vec2 dist = pfh_vec2_sub(to, from);
	return pfh_vec2_dot(dist, dist);
}

static inline float pfh_vec2_dist(pfh_vec2 from, pfh_vec2 to)
{
	pfh_vec2 dist = pfh_vec2_sub(to, from);
	return hypotf(dist.x, dist.y);
}

static inline pfh_vec2 pfh_vec2_lrp(pfh_vec2 from, pfh_vec2 to, float t)
{
	return pfh_vec2_add(from, pfh_vec2_mul_s(pfh_vec2_sub(to, from), t));
}

static inline pfh_vec2 pfh_vec2_rot_around(pfh_vec2 A, pfh_vec2 C, float r)
{
	const float s = sin(r);
	const float c = cos(r);

	const pfh_vec2 p = pfh_vec2_sub(A, C);
	const pfh_vec2 n = (pfh_vec2) {
		p.x * c - p.y * s,
		p.x * s + p.y * c
	};

	return pfh_vec2_add(n, C);
}

void pfh_get_stroke(pfh_vec2_buff *dest, const pfh_point pts[], size_t pts_len, const pfh_stroke_opts *opts)
{
	pfh_get_stroke_points(&pfh_buff_stroke_pts, pts, pts_len, opts);
	pfh_get_stroke_outline_points(dest, pfh_buff_stroke_pts.elems, pfh_buff_stroke_pts.len, opts);
}

/**
 * @brief Get an array of points as objects with an adjusted point, pressure, vector, distance, and running_length.
 */
void pfh_get_stroke_points(pfh_stroke_point_buff *dest, const pfh_point pts[], size_t pts_len, const pfh_stroke_opts *opts)
{
	if (pts_len == 0)
		return;

	// interpolation level between points.
	const float t = PFH_MIN_STREAMLINE_T + (1 - opts->streamline) * PFH_STREAMLINE_T_RANGE;

	/*
	// In the original, this was a part that fixes weirdness with tapering 
	// start & end when there's only two points by interpolates new ones.
	// fix urself, i'm not allocating shi
	*/

	dest->len = 0; // reset (maybe provide no reset opt?)
	pfh_buff_push(dest, ((pfh_stroke_point){
		.point = pts[0],
		.vector = {1, 1},
		.distance = 0,
		.running_length = 0,
	}));

	bool reached_min_len = false;
	float running_length = 0;

	size_t max = pts_len - 1;
	for (int i = 1; i < pts_len; i++) {
		pfh_stroke_point *prev = dest->elems + dest->len - 1;
		pfh_point point = (opts->is_complete && i == max) ?
			pts[pts_len-1] : // just add last point. otherwise, interpolate a new point with t
			(pfh_point){ pfh_vec2_lrp(prev->point.coord, pts[i].coord, t), pts[i].pressure };

		if (pfh_vec2_equal(prev->point.coord, point.coord)) 
			continue;

		float dist = pfh_vec2_dist(point.coord, pts[i].coord);
		running_length += dist;

		// Avoid noise
		if (i < max && !reached_min_len) {
			if (running_length < opts->size)
				continue;
			reached_min_len = true;
		}

		// Create a new pfh_stroke_point (it will be the new "previous" one).
		pfh_vec2 vec_diff = pfh_vec2_sub(prev->point.coord, point.coord);

		pfh_buff_push(dest, ((pfh_stroke_point){
			.point = point,
			.vector = pfh_vec2_uni(vec_diff),
			.distance = dist,
			.running_length = running_length,
		}));
	}

	dest->elems[0].vector = dest->len > 1 ? dest->elems[1].vector : (pfh_vec2){0, 0};
}

static inline void pfh_draw_dot(pfh_vec2_buff *dest, pfh_vec2 center, float radius)
{
	const pfh_vec2 offsetPoint = pfh_vec2_add(center, UNIT_OFFSET);
	const pfh_vec2 start = pfh_vec2_prj(center, pfh_vec2_uni(pfh_vec2_per(pfh_vec2_sub(center, offsetPoint))), -radius);

	float step = 1.0f / PFH_START_CAP_SEGMENTS;
	pfh_buff_reserve(dest, dest->len + PFH_START_CAP_SEGMENTS);
	for (float t = step; t <= 1; t += step)
		pfh_buff_push_raw(dest, pfh_vec2_rot_around(start, center, PFH_FIXED_PI * 2 * t));
}
static inline void pfh_draw_round_start_cap(pfh_vec2_buff *dest, pfh_vec2 center, pfh_vec2 right_point, float segments)
{
	float step = 1.0f / segments;

	pfh_buff_reserve(dest, dest->len + segments);
	for (float t = step; t <= 1; t += step)
		pfh_buff_push_raw(dest, pfh_vec2_rot_around(right_point, center, PFH_FIXED_PI * t));
}

static inline void pfh_draw_flat_start_cap(pfh_vec2_buff *dest, pfh_vec2 center, pfh_vec2 left_point, pfh_vec2 right_point)
{
	pfh_vec2 cornersVector = pfh_vec2_sub(left_point, right_point);
	pfh_vec2 offsetA = pfh_vec2_mul_s(cornersVector, 0.5f);
	pfh_vec2 offsetB = pfh_vec2_mul_s(cornersVector, 0.51f);

	pfh_buff_reserve(dest, dest->len + 4);
	pfh_buff_push_raw(dest, pfh_vec2_sub(center, offsetA));
	pfh_buff_push_raw(dest, pfh_vec2_sub(center, offsetB));
	pfh_buff_push_raw(dest, pfh_vec2_add(center, offsetB));
	pfh_buff_push_raw(dest, pfh_vec2_add(center, offsetA));
}

// 1.5 turns to handle sharp end turns correctly
static inline void pfh_draw_round_end_cap(pfh_vec2_buff *dest, pfh_vec2 center, pfh_vec2 direction, float radius, float segments)
{
	pfh_vec2 start = pfh_vec2_prj(center, direction, radius);
	float step = 1.0f / segments;

	pfh_buff_reserve(dest, dest->len + segments);
	for (float t = step; t < 1; t += step)
		pfh_buff_push(dest, pfh_vec2_rot_around(start, center, PFH_FIXED_PI * 3 * t));
}

static inline void pfh_draw_flat_end_cap(pfh_vec2_buff *dest, pfh_vec2 center, pfh_vec2 direction, float radius)
{
	pfh_buff_reserve(dest, dest->len + 4);
	pfh_buff_push_raw(dest, pfh_vec2_add(center, pfh_vec2_mul_s(direction, radius)));
	pfh_buff_push_raw(dest, pfh_vec2_add(center, pfh_vec2_mul_s(direction, radius * 0.99)));
	pfh_buff_push_raw(dest, pfh_vec2_sub(center, pfh_vec2_mul_s(direction, radius * 0.99)));
	pfh_buff_push_raw(dest, pfh_vec2_sub(center, pfh_vec2_mul_s(direction, radius)));
}

static inline float pfh_compute_taper_distance(float taper, float size, float total_length)
{
	if (taper == PFH_TAPER_NONE) return 0;
	if (taper == PFH_TAPER_MAX) return fmaxf(size, total_length);
	return taper;
}

static inline float pfh_simulate_pressure(float prev_pressure, float distance, float size)
{
	float speed = fminf(1, distance / size);
	float rate = fminf(1, 1 - speed);
	return fminf(
	    1,
	    prev_pressure + (rate - prev_pressure) * (speed * PFH_RATE_OF_PRESSURE_CHANGE
	));
}

/**
 * Compute by averaging the first few points.
 * This prevents "fat starts" since drawn lines almost always start slow.
 */
static inline float pfh_compute_initial_pressure(pfh_stroke_point pts[], size_t len, bool should_simulate_pressure, float size)
{
	float accumulator = pts[0].point.pressure;

	const int limit = len < 10 ? len : 10;
	for (int i = 0; i < limit; i++) {
		float curr = pts[i].point.pressure;
		if (should_simulate_pressure)
			curr = pfh_simulate_pressure(accumulator, pts[i].distance, size);
		accumulator = (accumulator + curr)/2.0f;
	}
	return accumulator;
}

static inline float pfh_easing(float t)
{
	return t;
}

static inline float pfh_easing_start(float t)
{
	return t * (2 - t);
}

static inline float pfh_easing_end(float t)
{
	return --t * t * t + 1;
}

static inline float pfh_get_stroke_radius(float size, float thinning, float pressure, pfh_easing_fn easing)
{
	float t = 0.5 - thinning * (0.5 - pressure);

	if (easing != NULL)
		return size * easing(t);
	else
		return size * pfh_easing(t);
}

/**
 * @brief Get an array of points representing the outline of a stroke.
 */
void pfh_get_stroke_outline_points(pfh_vec2_buff *dest, const pfh_stroke_point stroke_pts[], size_t stroke_pts_len, const pfh_stroke_opts *opts)
{

	pfh_vec2_buff *pfh_buff_leftpt = dest;

	if (stroke_pts_len == 0 || opts->size <= 0)
		return;

	const float total_length = stroke_pts[stroke_pts_len - 1].running_length;

	const float taper_start = pfh_compute_taper_distance(opts->start.taper, opts->size, total_length);
	const float taper_end = pfh_compute_taper_distance(opts->end.taper, opts->size, total_length);

	const double min_dist = pow(opts->size * opts->smoothing, 2); // The minimum allowed distance between stroke_pts (squared)

	pfh_vec2 left_pts[1];
	pfh_vec2 right_pts[1];

	float prev_pressure = pfh_compute_initial_pressure(
		stroke_pts,
		stroke_pts_len,
		opts->simulate_pressure,
		opts->size
	);

	float radius = pfh_get_stroke_radius(
		opts->size,
		opts->thinning,
		stroke_pts[stroke_pts_len - 1].point.pressure,
		opts->easing
	);

	float first_radius = -1;
	pfh_vec2 prev_vec = stroke_pts[0].vector;

	pfh_vec2 prev_left_pt = stroke_pts[0].point.coord;
	pfh_vec2 prev_right_pt = prev_left_pt;

	pfh_vec2 tmp_left_pt = prev_left_pt;
	pfh_vec2 tmp_right_pt = prev_right_pt;

	// Keep track of whether the previous point is a sharp corner
	// ... so that we don't detect the same corner twice
	bool prev_pt_is_sharp_corner = false;

	/*
	Find the outline's left and right stroke_pts
	skipping the first and last points, which will get caps later on.
	*/
	for (int i = 0; i < stroke_pts_len; i++) {
		bool is_last_pt = i == stroke_pts_len - 1;

		// Removes noise from the end of the line
		if (!is_last_pt && total_length - stroke_pts[i].running_length < PFH_END_NOISE_THRESHOLD)
			continue;

		/*
		Calculate the radius

		If not thinning, the current point's radius will be half the size; or
		otherwise, the size will be based on the current (real or simulated)
		pressure.
		*/
		if (opts->thinning) {
			float pressure = stroke_pts[i].point.pressure;
			if (opts->simulate_pressure)
				pressure = pfh_simulate_pressure(prev_pressure, stroke_pts[i].distance, opts->size);

			radius = pfh_get_stroke_radius(opts->size, opts->thinning, pressure, opts->easing);
		} else {
			radius = opts->size / 2;
		}

		if (first_radius == -1)
			first_radius = radius;

		/*
		Apply tapering

		If the current length is within the taper distance at either the
		start or the end, calculate the taper strengths. Apply the smaller
		of the two taper strengths to the radius.
		*/
		float taper_start_strength = 1;
		if (stroke_pts[i].running_length < taper_start) {
			float t = stroke_pts[i].running_length / taper_start;
			taper_start_strength = opts->start.easing
			? opts->start.easing(t)
			: pfh_easing_start(t);
		}

		float taper_end_strength = 1;
		if (total_length - stroke_pts[i].running_length < taper_end) {
			float t = (total_length - stroke_pts[i].running_length) / taper_end;
			taper_end_strength = opts->end.easing
			? opts->end.easing(t)
			: pfh_easing_end(t);
		}

		radius = fmaxf(
		    PFH_MIN_RADIUS,
		    radius * fminf(taper_start_strength, taper_end_strength)
		);

		/* Add stroke_pts to left and right */

		/*
		Handle sharp corners

		Find the difference (dot product) between the current and next vector.
		If the next vector is at more than a right angle to the current vector,
		draw a cap at the current point.
		*/

		pfh_vec2 next_vec = (!is_last_pt ? stroke_pts[i + 1] : stroke_pts[i]).vector;
		float next_dot = !is_last_pt ? pfh_vec2_dot(stroke_pts[i].vector, next_vec) : 1.0;
		float prev_dot = pfh_vec2_dot(stroke_pts[i].vector, prev_vec);

		bool pt_is_sharp_corner = prev_dot < 0 && !prev_pt_is_sharp_corner;
		bool next_pt_is_sharp_corner = !isnan(next_dot) && next_dot < 0; // originally it was checking next_dot != null? what?

		if (pt_is_sharp_corner || next_pt_is_sharp_corner) {
			// It's a sharp corner. Draw a rounded cap and move on to the next point
			pfh_vec2 offset = pfh_vec2_per(prev_vec);
			offset = pfh_vec2_mul_s(offset, radius);

			float step = 1.0f / PFH_CORNER_CAP_SEGMENTS;
			for (float t = 0; t <= 1; t += step) {
				tmp_left_pt = pfh_vec2_sub(stroke_pts[i].point.coord, offset);
				tmp_left_pt = pfh_vec2_rot_around(tmp_left_pt, stroke_pts[i].point.coord, PFH_FIXED_PI * t);
				pfh_buff_push(pfh_buff_leftpt, tmp_left_pt);

				tmp_right_pt = pfh_vec2_add(stroke_pts[i].point.coord, offset);
				tmp_right_pt = pfh_vec2_rot_around(tmp_right_pt, stroke_pts[i].point.coord, PFH_FIXED_PI * -t);
				pfh_buff_push(&pfh_buff_rightpt, tmp_right_pt);
			}

			prev_left_pt = tmp_left_pt;
			prev_right_pt = tmp_right_pt;

			if (next_pt_is_sharp_corner) {
				prev_pt_is_sharp_corner = true;
			}
			continue;
		}

		prev_pt_is_sharp_corner = false;

		// Handle the last point
		if (is_last_pt) {
			pfh_vec2 offset = pfh_vec2_per(stroke_pts[i].vector);
			offset = pfh_vec2_mul_s(offset, radius);

			pfh_vec2 sub = pfh_vec2_sub(stroke_pts[i].vector, offset);
			pfh_buff_push(pfh_buff_leftpt, sub);
			pfh_buff_push(&pfh_buff_rightpt, sub);
			continue;
		}

		/*
		Add regular stroke_pts

		Project stroke_pts to either side of the current point, using the
		calculated size as a distance. If a point's distance to the
		previous point on that side greater than the minimum distance
		(or if the corner is kinda sharp), add the stroke_pts to the side's
		stroke_pts array.
		*/

		// Use mutable operations for offset calculation
		pfh_vec2 offset = pfh_vec2_lrp(next_vec, stroke_pts[i].vector, next_dot);
		offset = pfh_vec2_per(offset);
		offset = pfh_vec2_mul_s(offset, radius);

		tmp_left_pt = pfh_vec2_sub(stroke_pts[i].point.coord, offset);

		if (i <= 1 || pfh_vec2_dist2(prev_left_pt, tmp_left_pt) > min_dist) {
			pfh_buff_push(pfh_buff_leftpt, tmp_left_pt);
			prev_left_pt = tmp_left_pt;
		}

		tmp_right_pt = pfh_vec2_add(stroke_pts[i].point.coord, offset);

		if (i <= 1 || pfh_vec2_dist2(prev_right_pt, tmp_right_pt) > min_dist) {
			pfh_buff_push(&pfh_buff_rightpt, tmp_right_pt);
			prev_right_pt = tmp_right_pt;
		}

		prev_pressure = stroke_pts[i].point.pressure;
		prev_vec = stroke_pts[i].vector;
	}

	/*
	Drawing caps

	Now that we have our stroke_pts on either side of the line, we need to
	draw caps at the start and end. Tapered lines don't have caps, but
	may have dots for very short lines.
	*/

	pfh_vec2 first_point = stroke_pts[0].point.coord;

	pfh_vec2 last_point =
	    stroke_pts_len > 1
	    ? stroke_pts[stroke_pts_len - 1].point.coord
	    : pfh_vec2_add(stroke_pts[0].point.coord, UNIT_OFFSET);
	
	if (stroke_pts_len == 1) {
		if (!(taper_start || taper_end) || opts->is_complete) {
			pfh_draw_dot(dest, first_point, first_radius || radius);
			return;
		}
	} else {
		// Draw start cap (unless tapered)
		if (taper_start || (taper_end && stroke_pts_len == 1)) {
			// The start point is tapered, noop
		} else if (opts->start.cap) {
			pfh_draw_round_start_cap(
				&dest,
				first_point, 
				pfh_buff_rightpt.elems[0], 
				PFH_START_CAP_SEGMENTS
			);
		} else {
			pfh_draw_flat_start_cap(
				&dest,
				first_point,
				pfh_buff_leftpt->elems[0], 
				pfh_buff_rightpt.elems[0]
			);
		}

		// Draw end cap (unless tapered)
		pfh_vec2 direction = pfh_vec2_per(pfh_vec2_neg(stroke_pts[stroke_pts_len - 1].vector));

		if (taper_end || (taper_start && stroke_pts_len == 1)) {
			// Tapered end - push the last point to the line
			pfh_buff_push(&pfh_buff_endcap, last_point);
		} else if (opts->end.cap) {
			pfh_draw_round_end_cap(
				&pfh_buff_endcap,
				last_point,
				direction,
				radius,
				PFH_END_CAP_SEGMENTS
			);
		} else {
			pfh_draw_flat_end_cap(&pfh_buff_endcap, last_point, direction, radius);
		}
	}

	/*
	Return the stroke_pts in the correct winding order: begin on the left side, then
	continue around the end cap, then come back along the right side, and finally
	complete the start cap.
	*/
	// dest === _pft_leftpt_buff
	pfh_buff_left_concat(dest, &pfh_buff_endcap);
	pfh_buff_left_concat_reverse(dest, &pfh_buff_rightpt);
	pfh_buff_left_concat(dest, &pfh_buff_startcap);
}

#endif
#endif

// TODO: 
// DEFAULT_PRESSURE for the first point needs to be set. 
// Make util function to modify pressures
