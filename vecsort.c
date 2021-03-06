/** \file
 * Topologically sort vectors for faster laser cutting or plotting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef struct _vector vector_t;
struct _vector
{
	vector_t * next;
	vector_t ** prev;
	double x1;
	double y1;
	double x2;
	double y2;
};


typedef struct
{
	vector_t * vectors;
} vectors_t;

// Red/Green/Blue
#define VECTOR_PASSES 3

// close enough for floating point work
static inline int fpeq(double x, double y)
{
	const double eps = 1e-8;
	return fabs(x-y) < eps;
}


static double
vector_transit_len(
	vector_t * v
)
{
	double lx = 0;
	double ly = 0;

	double transit_len_sum = 0;

	while (v)
	{
		double t_dx = lx - v->x1;
		double t_dy = ly - v->y1;

		double transit_len = sqrt(t_dx * t_dx + t_dy * t_dy);
		if (transit_len != 0)
			transit_len_sum += transit_len;

		// Advance the point
		lx = v->x2;
		ly = v->y2;
		v = v->next;
	}

	return transit_len_sum;
}


static void
vector_stats(
	vector_t * v
)
{
	double lx = 0;
	double ly = 0;
	double cut_len_sum = 0;
	int cuts = 0;

	double transit_len_sum = 0;
	int transits = 0;

	while (v)
	{
		double t_dx = lx - v->x1;
		double t_dy = ly - v->y1;

		double transit_len = sqrt(t_dx * t_dx + t_dy * t_dy);
		if (transit_len != 0)
		{
			transits++;
			transit_len_sum += transit_len;
		}

		double c_dx = v->x1 - v->x2;
		double c_dy = v->y1 - v->y2;

		double cut_len = sqrt(c_dx*c_dx + c_dy*c_dy);
		if (cut_len != 0)
		{
			cuts++;
			cut_len_sum += cut_len;
		}

		// Advance the point
		lx = v->x2;
		ly = v->y2;
		v = v->next;
	}

	fprintf(stderr, "Cuts: %u len %.0f\n", cuts, cut_len_sum);
	fprintf(stderr, "Move: %u len %.0f\n", transits, transit_len_sum);
}


static void
vector_create(
	vectors_t * const vectors,
	double x1,
	double y1,
	double x2,
	double y2
)
{
	// Find the end of the list and, if vector optimization is
	// turned on, check for duplicates
	vector_t ** iter = &vectors->vectors;
	while (*iter)
	{
		vector_t * const p = *iter;

		if (fpeq(p->x1,x1) && fpeq(p->y1,y1)
		&&  fpeq(p->x2,x2) && fpeq(p->y2,y2))
			return;

		if (fpeq(p->x1,x2) && fpeq(p->y1,y2)
		&&  fpeq(p->x2,x1) && fpeq(p->y2,y1))
			return;

		if (fpeq(x1,x2) && fpeq(y1,y2))
			return;

		iter = &p->next;
	}

	vector_t * const v = calloc(1, sizeof(*v));
	if (!v)
		return;

	v->x1 = x1;
	v->y1 = y1;
	v->x2 = x2;
	v->y2 = y2;

	// Append it to the now known end of the list
	v->next = NULL;
	v->prev = iter;
	*iter = v;
}



/**
 * Generate a list of vectors.
 *
 * The vector format is:
 * P r,g,b -- Color of the vector
 * M x,y -- Move (start a line at x,y)
 * L x,y -- Line to x,y from the current position
 * Z -- Closing line segment to the starting position
 *
 * Multi segment vectors are split into individual vectors, which are
 * then passed into the topological sort routine.
 *
 * Exact duplictes will be deleted to try to avoid double hits..
 */
static vectors_t *
vectors_parse(
	FILE * const vector_file
)
{
	vectors_t * const vectors = calloc(VECTOR_PASSES, sizeof(*vectors));
	double mx = 0, my = 0;
	double lx = 0, ly = 0;
	int pass = 0;
	int count = 0;

	char buf[256];

	while (fgets(buf, sizeof(buf), vector_file))
	{
		//fprintf(stderr, "read '%s'\n", buf);
		const char cmd = buf[0];
		double x, y;

		switch (cmd)
		{
		case 'P':
		{
			// note that they will be in bgr order in the file
			int r, g, b;
			sscanf(buf+1, "%d %d %d", &b, &g, &r);
			if (r == 0 && g != 0 && b == 0)
			{
				pass = 0;
			} else
			if (r != 0 && g == 0 && b == 0)
			{
				pass = 1;
			} else
			if (r == 0 && g == 0 && b != 0)
			{
				pass = 2;
			} else {
				fprintf(stderr, "non-red/green/blue vector? %d,%d,%d\n", r, g, b);
				exit(-1);
			}
			break;
		}
		case 'M':
			// Start a new line.
			// This also implicitly sets the
			// current laser position
			sscanf(buf+1, "%lf %lf", &mx, &my);
			lx = mx;
			ly = my;
			break;
		case 'L':
			// Add a line segment from the current
			// point to the new point, and update
			// the current point to the new point.
			sscanf(buf+1, "%lf %lf", &x, &y);
			vector_create(&vectors[pass], lx, ly, x, y);
			count++;
			lx = x;
			ly = y;
			break;
		case 'Z':
			// Closing segment from the current point
			// back to the starting point
			vector_create(&vectors[pass], lx, ly, mx, my);
			lx = mx;
			lx = my;
			break;
		case 'X':
			goto done;
		default:
			fprintf(stderr, "Unknown command '%c'", cmd);
			return NULL;
		}
	}

done:
	fprintf(stderr, "read %u segments\n", count);
/*
	for (int i = 0 ; i < VECTOR_PASSES ; i++)
	{
		vector_stats(vectors[i].vectors);
	}

	fprintf(stderr, "---\n");
*/

	return vectors;
}


/** Find the closest vector to a given point and remove it from the list.
 *
 * This might reverse a vector if it is closest to draw it in reverse
 * order.
 */
static vector_t *
vector_find_closest(
	vector_t * v,
	const double cx,
	const double cy
)
{
	double best_dist = 1e9;
	vector_t * best = NULL;
	int do_reverse = 0;

	while (v)
	{
		double dx1 = cx - v->x1;
		double dy1 = cy - v->y1;
		double dist1 = dx1*dx1 + dy1*dy1;

		if (dist1 < best_dist)
		{
			best = v;
			best_dist = dist1;
			do_reverse = 0;
		}

		double dx2 = cx - v->x2;
		double dy2 = cy - v->y2;
		double dist2 = dx2*dx2 + dy2*dy2;
		if (dist2 < best_dist)
		{
			best = v;
			best_dist = dist2;
			do_reverse = 1;
		}

		v = v->next;
	}

	if (!best)
		return NULL;

	// Remove it from the list
	if (best->prev)
		*best->prev = best->next;
	if (best->next)
		best->next->prev = best->prev;

	// If reversing is required, flip the x1/x2 and y1/y2
	if (do_reverse)
	{
		double x1 = best->x1;
		double y1 = best->y1;
		best->x1 = best->x2;
		best->y1 = best->y2;
		best->x2 = x1;
		best->y2 = y1;
	}

	best->next = NULL;
	best->prev = NULL;

	return best;
}


/**
 * Optimize the cut order to minimize transit time.
 *
 * Simplistic greedy algorithm: look for the closest vector that starts
 * or ends at the same point as the current point. 
 *
 * This does not split vectors.
 */
static vector_t *
vector_optimize(
	vector_t ** vectors,
	double *cx_ptr,
	double *cy_ptr
)
{
	vector_t * vs = NULL;
	vector_t * vs_tail = NULL;
	double cx = *cx_ptr;
	double cy = *cy_ptr;

	while (*vectors)
	{
		vector_t * v = vector_find_closest(*vectors, cx, cy);
		if (!v)
		{
			fprintf(stderr, "nothing close?\n");
			abort();
		}

		if (!vs)
		{
			// Nothing on the list yet
			vs = vs_tail = v;
		} else {
			// Add it to the tail of the list
			v->next = NULL;
			v->prev = &vs_tail->next;
			vs_tail->next = v;
			vs_tail = v;
		}
		
		// Move the current point to the end of the line segment
		cx = v->x2;
		cy = v->y2;
	}

	//vector_stats(vs);
	*cx_ptr = cx;
	*cy_ptr = cy;

	// update the pointers
	*vectors = vs;
	if (vs)
		vs->prev = vectors;

	return vs;
}


/*
 * Attempt to remove long transits.
 * Find the longest transit and attempt to move that point
 * to a closer point.  Then check to see if it reduces the transit.
 * returns the reduction in distance
 */
static double
vector_refine(
	vector_t * const vector,
	double *cx_ptr,
	double *cy_ptr
)
{
	const double initial_transit_len = vector_transit_len(vector);
	double cx = *cx_ptr;
	double cy = *cy_ptr;

	// find the longest transit
	vector_t * v = vector;
	double max_transit = 0;
	vector_t * transit_v = NULL;

	while (v)
	{
		double t_dx = cx - v->x1;
		double t_dy = cy - v->y1;

		double transit_len = sqrt(t_dx * t_dx + t_dy * t_dy);
		if (!fpeq(transit_len, 0) && max_transit < transit_len)
		{
			max_transit = transit_len;
			transit_v = v;
		}

		// Advance the point
		cx = v->x2;
		cy = v->y2;
		v = v->next;
	}

	if (!transit_v)
	{
		fprintf(stderr, "no longest transit?\n");
		return 0;
	}

	fprintf(stderr, "Total transit: %.3f\n", initial_transit_len);
	fprintf(stderr, "longest transit: %.3f: %.3f,%.3f\n",
		max_transit,
		transit_v->x1,
		transit_v->y1
	);

	// then find the closest *end point* prior to this transit
	vector_t * closest = NULL;
	double min_dist = 1e9;
	for(vector_t * v = vector ; v != transit_v ; v = v->next)
	{
		double dx = v->x2 - transit_v->x1;
		double dy = v->y2 - transit_v->y1;
		double dist = dx*dx + dy*dy;
		if (min_dist < dist)
			continue;
		min_dist = dist;
		closest = v;
	}

	if (!closest)
	{
		fprintf(stderr, "could not find a close one?\n");
		return 0;
	}

	// move the longest transit destination to come after the
	// one closest to it, then re-sort based on that point

	// remove transit_v from the list
	if (transit_v->next)
		transit_v->next->prev = transit_v->prev;
	if (transit_v->prev)
		*transit_v->prev = transit_v->next;

	// re-insert it after the closest one
	transit_v->next = closest->next;
	transit_v->prev = &closest->next;
	closest->next = transit_v;
	if (transit_v->next)
		transit_v->next->prev = &transit_v->next;

	// now sort the ones that come after it
	double cx2 = transit_v->x2;
	double cy2 = transit_v->y2;
	vector_optimize(&transit_v->next, &cx2, &cy2);

	const double new_transit_len = vector_transit_len(vector);
	fprintf(stderr, "Refine transit %.3f\n", new_transit_len);

	*cx_ptr = cx2;
	*cy_ptr = cy2;

	return initial_transit_len - new_transit_len;
}


static void
output_vector(
	FILE * const pjl_file,
	const vector_t * v
)
{
	double lx = 0;
	double ly = 0;

	while (v)
	{
		if (fpeq(v->x1,lx) && fpeq(v->y1,ly))
		{
			// This is the continuation of a line, so
			// just add additional points
			fprintf(pjl_file, "L %.3f %.3f\n",
				v->x2,
				v->y2
			);
		} else {
			// Stop the laser; we need to transit
			// and then start the laser as we go to
			// the next point.  Note initial ";"
			fprintf(pjl_file, "\nM %.3f %.3f\nL %.3f %.3f\n",
				v->x1,
				v->y1,
				v->x2,
				v->y2
			);
		}

		// Changing power on the fly is not supported for now
		// \todo: Check v->power and adjust ZS, XR, etc

		// Move to the next vector, updating our current point
		lx = v->x2;
		ly = v->y2;
		v = v->next;
	}
	fprintf(pjl_file, "\n");
}

				
static void
generate_vectors(
	FILE * const vector_file,
	FILE * const pjl_file
)
{
	vectors_t * const vectors = vectors_parse(vector_file);
	double lx = 0;
	double ly = 0;

	for (int i = 0 ; i < VECTOR_PASSES ; i++)
	{
		vectors_t * const vs = &vectors[i];
		if (!vs->vectors)
			continue;

		fprintf(stderr, "Group %d\n", i);
		vector_stats(vs->vectors);
		vector_optimize(
			&vs->vectors,
			&lx, &ly
		);

		vector_stats(vs->vectors);

/*
		for(int i = 0 ; i < 8 ; i++)
		{
			double sx = vs->vectors->x1;
			double sy = vs->vectors->y1;
			if (vector_refine(vs->vectors, &sx, &sy) <= 0)
				break;
			lx = sx;
			ly = sy;
		}
*/


		fprintf(pjl_file, "P %d %d %d\n",
			i == 0 ? 100 : 0,
			i == 1 ? 100 : 0,
			i == 2 ? 100 : 0
		);
		output_vector(pjl_file, vs->vectors);
		fprintf(pjl_file, "\n\n");
	}
}


int main(void)
{
	generate_vectors(stdin, stdout);
	return 0;
}
