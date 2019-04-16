// SPDX-License-Identifier: GPL-2.0
#include "ssrf.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <git2.h>
#include <libdivecomputer/parser.h>

#include "gettext.h"

#include "divesite.h"
#include "subsurface-string.h"
#include "divelist.h"
#include "device.h"
#include "membuffer.h"
#include "git-access.h"
#include "qthelper.h"

const char *saved_git_id = NULL;

struct keyword_action {
	const char *keyword;
	void (*fn)(char *, struct membuffer *, void *);
};
#define ARRAY_SIZE(array) (sizeof(array)/sizeof(array[0]))

git_blob *git_tree_entry_blob(git_repository *repo, const git_tree_entry *entry);

static char *get_utf8(struct membuffer *b)
{
	int len = b->len;
	char *res;

	if (!len)
		return NULL;
	res = malloc(len+1);
	if (res) {
		memcpy(res, b->buffer, len);
		res[len] = 0;
	}
	return res;
}

static temperature_t get_temperature(const char *line)
{
	temperature_t t;
	t.mkelvin = C_to_mkelvin(ascii_strtod(line, NULL));
	return t;
}

static depth_t get_depth(const char *line)
{
	depth_t d;
	d.mm = lrint(1000*ascii_strtod(line, NULL));
	return d;
}

static volume_t get_volume(const char *line)
{
	volume_t v;
	v.mliter = lrint(1000*ascii_strtod(line, NULL));
	return v;
}

static weight_t get_weight(const char *line)
{
	weight_t w;
	w.grams = lrint(1000*ascii_strtod(line, NULL));
	return w;
}

static pressure_t get_pressure(const char *line)
{
	pressure_t p;
	p.mbar = lrint(1000*ascii_strtod(line, NULL));
	return p;
}

static int get_salinity(const char *line)
{
	return lrint(10*ascii_strtod(line, NULL));
}

static fraction_t get_fraction(const char *line)
{
	fraction_t f;
	f.permille = lrint(10*ascii_strtod(line, NULL));
	return f;
}

static void update_date(timestamp_t *when, const char *line)
{
	unsigned yyyy, mm, dd;
	struct tm tm;

	if (sscanf(line, "%04u-%02u-%02u", &yyyy, &mm, &dd) != 3)
		return;
	utc_mkdate(*when, &tm);
	tm.tm_year = yyyy;
	tm.tm_mon = mm - 1;
	tm.tm_mday = dd;
	*when = utc_mktime(&tm);
}

static void update_time(timestamp_t *when, const char *line)
{
	unsigned h, m, s = 0;
	struct tm tm;

	if (sscanf(line, "%02u:%02u:%02u", &h, &m, &s) < 2)
		return;
	utc_mkdate(*when, &tm);
	tm.tm_hour = h;
	tm.tm_min = m;
	tm.tm_sec = s;
	*when = utc_mktime(&tm);
}

static duration_t get_duration(const char *line)
{
	int m = 0, s = 0;
	duration_t d;
	sscanf(line, "%d:%d", &m, &s);
	d.seconds = m*60+s;
	return d;
}

static enum divemode_t get_dctype(const char *line)
{
	for (enum divemode_t i = 0; i < NUM_DIVEMODE; i++) {
		if (strcmp(line, divemode_text[i]) == 0)
			return i;
	}
	return 0;
}

static int get_index(const char *line)
{ return atoi(line); }

static int get_hex(const char *line)
{ return strtoul(line, NULL, 16); }

static void parse_dive_gps(char *line, struct membuffer *str, void *_dive)
{
	UNUSED(str);
	location_t location;
	struct dive *dive = _dive;
	struct dive_site *ds = get_dive_site_for_dive(dive);

	parse_location(line, &location);
	if (!ds) {
		ds = get_dive_site_by_gps(&location, &dive_site_table);
		if (!ds)
			ds = create_dive_site_with_gps("", &location, &dive_site_table);
		add_dive_to_dive_site(dive, ds);
	} else {
		if (dive_site_has_gps_location(ds) && !same_location(&ds->location, &location)) {
			char *coords = printGPSCoordsC(&location);
			// we have a dive site that already has GPS coordinates
			ds->notes = add_to_string(ds->notes, translate("gettextFromC", "multiple GPS locations for this dive site; also %s\n"), coords);
			free(coords);
		}
		ds->location = location;
	}

}

static void parse_dive_location(char *line, struct membuffer *str, void *_dive)
{
	UNUSED(line);
	char *name = get_utf8(str);
	struct dive *dive = _dive;
	struct dive_site *ds = get_dive_site_for_dive(dive);
	if (!ds) {
		ds = get_dive_site_by_name(name, &dive_site_table);
		if (!ds)
			ds = create_dive_site(name, &dive_site_table);
		add_dive_to_dive_site(dive, ds);
	} else {
		// we already had a dive site linked to the dive
		if (empty_string(ds->name)) {
			ds->name = strdup(name);
		} else {
			// and that dive site had a name. that's weird - if our name is different, add it to the notes
			if (!same_string(ds->name, name))
				ds->notes = add_to_string(ds->notes, translate("gettextFromC", "additional name for site: %s\n"), name);
		}
	}
	free(name);
}

static void parse_dive_divemaster(char *line, struct membuffer *str, void *_dive)
{ UNUSED(line); struct dive *dive = _dive; dive->divemaster = get_utf8(str); }

static void parse_dive_buddy(char *line, struct membuffer *str, void *_dive)
{ UNUSED(line); struct dive *dive = _dive; dive->buddy = get_utf8(str); }

static void parse_dive_suit(char *line, struct membuffer *str, void *_dive)
{ UNUSED(line); struct dive *dive = _dive; dive->suit = get_utf8(str); }

static void parse_dive_notes(char *line, struct membuffer *str, void *_dive)
{ UNUSED(line); struct dive *dive = _dive; dive->notes = get_utf8(str); }

static void parse_dive_divesiteid(char *line, struct membuffer *str, void *_dive)
{ UNUSED(str); struct dive *dive = _dive; add_dive_to_dive_site(dive, get_dive_site_by_uuid(get_hex(line), &dive_site_table)); }

/*
 * We can have multiple tags in the membuffer. They are separated by
 * NUL bytes.
 */
static void parse_dive_tags(char *line, struct membuffer *str, void *_dive)
{
	UNUSED(line);
	struct dive *dive = _dive;
	const char *tag;
	int len = str->len;

	if (!len)
		return;

	/* Make sure there is a NUL at the end too */
	tag = mb_cstring(str);
	for (;;) {
		int taglen = strlen(tag);
		if (taglen)
			taglist_add_tag(&dive->tag_list, tag);
		len -= taglen;
		if (!len)
			return;
		tag += taglen+1;
		len--;
	}
}

static void parse_dive_airtemp(char *line, struct membuffer *str, void *_dive)
{ UNUSED(str); struct dive *dive = _dive; dive->airtemp = get_temperature(line); }

static void parse_dive_watertemp(char *line, struct membuffer *str, void *_dive)
{ UNUSED(str); struct dive *dive = _dive; dive->watertemp = get_temperature(line); }

static void parse_dive_duration(char *line, struct membuffer *str, void *_dive)
{ UNUSED(str); struct dive *dive = _dive; dive->duration = get_duration(line); }

static void parse_dive_rating(char *line, struct membuffer *str, void *_dive)
{ UNUSED(str); struct dive *dive = _dive; dive->rating = get_index(line); }

static void parse_dive_visibility(char *line, struct membuffer *str, void *_dive)
{ UNUSED(str); struct dive *dive = _dive; dive->visibility = get_index(line); }

static void parse_dive_notrip(char *line, struct membuffer *str, void *_dive)
{
	UNUSED(str);
	UNUSED(line);
	struct dive *dive = _dive; dive->notrip = true;
}

static void parse_site_description(char *line, struct membuffer *str, void *_ds)
{ UNUSED(line); struct dive_site *ds = _ds; ds->description = strdup(mb_cstring(str)); }

static void parse_site_name(char *line, struct membuffer *str, void *_ds)
{ UNUSED(line); struct dive_site *ds = _ds; ds->name = strdup(mb_cstring(str)); }

static void parse_site_notes(char *line, struct membuffer *str, void *_ds)
{ UNUSED(line); struct dive_site *ds = _ds; ds->notes = strdup(mb_cstring(str)); }

static void parse_site_gps(char *line, struct membuffer *str, void *_ds)
{
	UNUSED(str);
	struct dive_site *ds = _ds;

	parse_location(line, &ds->location);
}

static void parse_site_geo(char *line, struct membuffer *str, void *_ds)
{
	struct dive_site *ds = _ds;
	if (ds->taxonomy.category == NULL)
		ds->taxonomy.category = alloc_taxonomy();
	int nr = ds->taxonomy.nr;
	if (nr < TC_NR_CATEGORIES) {
		struct taxonomy *t = &ds->taxonomy.category[nr];
		t->value = strdup(mb_cstring(str));
		sscanf(line, "cat %d origin %d \"", &t->category, (int *)&t->origin);
		ds->taxonomy.nr++;
	}
}

/* Parse key=val parts of samples and cylinders etc */
static char *parse_keyvalue_entry(void (*fn)(void *, const char *, const char *), void *fndata, char *line)
{
	char *key = line, *val, c;

	while ((c = *line) != 0) {
		if (isspace(c) || c == '=')
			break;
		line++;
	}

	if (c == '=')
		*line++ = 0;
	val = line;

	while ((c = *line) != 0) {
		if (isspace(c))
			break;
		line++;
	}
	if (c)
		*line++ = 0;

	fn(fndata, key, val);
	return line;
}

static int cylinder_index, weightsystem_index;
static int o2pressure_sensor;

static void parse_cylinder_keyvalue(void *_cylinder, const char *key, const char *value)
{
	cylinder_t *cylinder = _cylinder;
	if (!strcmp(key, "vol")) {
		cylinder->type.size = get_volume(value);
		return;
	}
	if (!strcmp(key, "workpressure")) {
		cylinder->type.workingpressure = get_pressure(value);
		return;
	}
	/* This is handled by the "get_utf8()" */
	if (!strcmp(key, "description"))
		return;
	if (!strcmp(key, "o2")) {
		cylinder->gasmix.o2 = get_fraction(value);
		return;
	}
	if (!strcmp(key, "he")) {
		cylinder->gasmix.he = get_fraction(value);
		return;
	}
	if (!strcmp(key, "start")) {
		cylinder->start = get_pressure(value);
		return;
	}
	if (!strcmp(key, "end")) {
		cylinder->end = get_pressure(value);
		return;
	}
	if (!strcmp(key, "use")) {
		cylinder->cylinder_use = cylinderuse_from_text(value);
		return;
	}
	if (!strcmp(key, "depth")) {
		cylinder->depth = get_depth(value);
		return;
	}
	if ((*key == 'm') && strlen(value) == 0) {
		/* found a bogus key/value pair in the cylinder, consisting
		 * of a lonely "m" or m<single quote> without value. This
		 * is caused by commit 46004c39e26 and fixed in 48d9c8eb6eb0 and
		 * b984fb98c38e4. See referenced commits for more info.
		 *
		 * Just ignore this key/value pair. No processing is broken
		 * due to this, as the git storage stores only metric SI type data.
		 * In fact, the m unit is superfluous anyway.
		 */
		return;
	}
	report_error("Unknown cylinder key/value pair (%s/%s)", key, value);
}

static void parse_dive_cylinder(char *line, struct membuffer *str, void *_dive)
{
	struct dive *dive = _dive;
	cylinder_t *cylinder = dive->cylinder + cylinder_index;

	cylinder->type.description = get_utf8(str);
	for (;;) {
		char c;
		while (isspace(c = *line))
			line++;
		if (!c)
			break;
		line = parse_keyvalue_entry(parse_cylinder_keyvalue, cylinder, line);
	}
	if (cylinder->cylinder_use == OXYGEN)
		o2pressure_sensor = cylinder_index;
	cylinder_index++;
}

static void parse_weightsystem_keyvalue(void *_ws, const char *key, const char *value)
{
	weightsystem_t *ws = _ws;
	if (!strcmp(key, "weight")) {
		ws->weight = get_weight(value);
		return;
	}
	/* This is handled by the "get_utf8()" */
	if (!strcmp(key, "description"))
		return;
	report_error("Unknown weightsystem key/value pair (%s/%s)", key, value);
}

static void parse_dive_weightsystem(char *line, struct membuffer *str, void *_dive)
{
	struct dive *dive = _dive;
	weightsystem_t *ws = dive->weightsystem + weightsystem_index;

	weightsystem_index++;
	ws->description = get_utf8(str);
	for (;;) {
		char c;
		while (isspace(c = *line))
			line++;
		if (!c)
			break;
		line = parse_keyvalue_entry(parse_weightsystem_keyvalue, ws, line);
	}
}

static int match_action(char *line, struct membuffer *str, void *data,
	struct keyword_action *action, unsigned nr_action)
{
	char *p = line, c;
	unsigned low, high;

	while ((c = *p) >= 'a' && c <= 'z') // skip over 1st word
		p++;	// Extract the second word from the line:
	if (p == line)
		return -1;
	switch (c) {
	case 0:		// if 2nd word is C-terminated
		break;
	case ' ':	// =end of 2nd word?
		*p++ = 0;	// then C-terminate that word
		break;
	default:
		return -1;
	}

	/* Standard binary search in a table */
	low = 0;
	high = nr_action;
	while (low < high) {
		unsigned mid = (low+high)/2;
		struct keyword_action *a = action + mid;
		int cmp = strcmp(line, a->keyword);
		if (!cmp) {	// attribute found:
			a->fn(p, str, data);	// Execute appropriate function,
			return 0;		// .. passing 2n word from above
		}				// (p) as a function argument.
		if (cmp < 0)
			high = mid;
		else
			low = mid+1;
	}
report_error("Unmatched action '%s'", line);
	return -1;
}

/* FIXME! We should do the array thing here too. */
static void parse_sample_keyvalue(void *_sample, const char *key, const char *value)
{
	struct sample *sample = _sample;

	if (!strcmp(key, "sensor")) {
		sample->sensor[0] = atoi(value);
		return;
	}
	if (!strcmp(key, "ndl")) {
		sample->ndl = get_duration(value);
		return;
	}
	if (!strcmp(key, "tts")) {
		sample->tts = get_duration(value);
		return;
	}
	if (!strcmp(key, "in_deco")) {
		sample->in_deco = atoi(value);
		return;
	}
	if (!strcmp(key, "stoptime")) {
		sample->stoptime = get_duration(value);
		return;
	}
	if (!strcmp(key, "stopdepth")) {
		sample->stopdepth = get_depth(value);
		return;
	}
	if (!strcmp(key, "cns")) {
		sample->cns = atoi(value);
		return;
	}

	if (!strcmp(key, "rbt")) {
		sample->rbt = get_duration(value);
		return;
	}

	if (!strcmp(key, "po2")) {
		pressure_t p = get_pressure(value);
		sample->setpoint.mbar = p.mbar;
		return;
	}
	if (!strcmp(key, "sensor1")) {
		pressure_t p = get_pressure(value);
		sample->o2sensor[0].mbar = p.mbar;
		return;
	}
	if (!strcmp(key, "sensor2")) {
		pressure_t p = get_pressure(value);
		sample->o2sensor[1].mbar = p.mbar;
		return;
	}
	if (!strcmp(key, "sensor3")) {
		pressure_t p = get_pressure(value);
		sample->o2sensor[2].mbar = p.mbar;
		return;
	}
	if (!strcmp(key, "o2pressure")) {
		pressure_t p = get_pressure(value);
		sample->pressure[1].mbar = p.mbar;
		return;
	}
	if (!strcmp(key, "heartbeat")) {
		sample->heartbeat = atoi(value);
		return;
	}
	if (!strcmp(key, "bearing")) {
		sample->bearing.degrees = atoi(value);
		return;
	}

	report_error("Unexpected sample key/value pair (%s/%s)", key, value);
}

static char *parse_sample_unit(struct sample *sample, double val, char *unit)
{
	unsigned int sensor;
	char *end = unit, c;

	/* Skip over the unit */
	while ((c = *end) != 0) {
		if (isspace(c)) {
			*end++ = 0;
			break;
		}
		end++;
	}

	/* The units are "°C", "m" or "bar", so let's just look at the first character */
	/* The cylinder pressure may also be of the form '123.0bar:4' to indicate sensor */
	switch (*unit) {
	case 'm':
		sample->depth.mm = lrint(1000*val);
		break;
	case 'b':
		sensor = sample->sensor[0];
		if (end > unit+4 && unit[3] == ':')
			sensor = atoi(unit+4);
		add_sample_pressure(sample, sensor, lrint(1000*val));
		break;
	default:
		sample->temperature.mkelvin = C_to_mkelvin(val);
		break;
	}

	return end;
}

/*
 * By default the sample data does not change unless the
 * save-file gives an explicit new value. So we copy the
 * data from the previous sample if one exists, and then
 * the parsing will update it as necessary.
 *
 * There are a few exceptions, like the sample pressure:
 * missing sample pressure doesn't mean "same as last
 * time", but "interpolate". We clear those ones
 * explicitly.
 *
 * NOTE! We default sensor use to 0, 1 respetively for
 * the two sensors, but for CCR dives with explicit
 * OXYGEN bottles we set the secondary sensor to that.
 * Then the primary sensor will be either the first
 * or the second cylinder depending on what isn't an
 * oxygen cylinder.
 */
static struct sample *new_sample(struct divecomputer *dc)
{
	struct sample *sample = prepare_sample(dc);
	if (sample != dc->sample) {
		memcpy(sample, sample-1, sizeof(struct sample));
		sample->pressure[0].mbar = 0;
		sample->pressure[1].mbar = 0;
	} else {
		sample->sensor[0] = !o2pressure_sensor;
		sample->sensor[1] = o2pressure_sensor;
	}
	return sample;
}

static void sample_parser(char *line, struct divecomputer *dc)
{
	int m, s = 0;
	struct sample *sample = new_sample(dc);

	m = strtol(line, &line, 10);
	if (*line == ':')
		s = strtol(line+1, &line, 10);
	sample->time.seconds = m*60+s;

	for (;;) {
		char c;

		while (isspace(c = *line))
			line++;
		if (!c)
			break;
		/* Less common sample entries have a name */
		if (c >= 'a' && c <= 'z') {
			line = parse_keyvalue_entry(parse_sample_keyvalue, sample, line);
		} else {
			const char *end;
			double val = ascii_strtod(line, &end);
			if (end == line) {
				report_error("Odd sample data: %s", line);
				break;
			}
			line = (char *)end;
			line = parse_sample_unit(sample, val, line);
		}
	}
	finish_sample(dc);
}

static void parse_dc_airtemp(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->airtemp = get_temperature(line); }

static void parse_dc_date(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; update_date(&dc->when, line); }

static void parse_dc_deviceid(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; set_dc_deviceid(dc, get_hex(line)); }

static void parse_dc_diveid(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->diveid = get_hex(line); }

static void parse_dc_duration(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->duration = get_duration(line); }

static void parse_dc_dctype(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->divemode = get_dctype(line); }

static void parse_dc_lastmanualtime(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->last_manual_time = get_duration(line); }

static void parse_dc_maxdepth(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->maxdepth = get_depth(line); }

static void parse_dc_meandepth(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->meandepth = get_depth(line); }

static void parse_dc_model(char *line, struct membuffer *str, void *_dc)
{ UNUSED(line); struct divecomputer *dc = _dc; dc->model = get_utf8(str); }

static void parse_dc_numberofoxygensensors(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->no_o2sensors = get_index(line); }

static void parse_dc_surfacepressure(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->surface_pressure = get_pressure(line); }

static void parse_dc_salinity(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->salinity = get_salinity(line); }

static void parse_dc_surfacetime(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->surfacetime = get_duration(line); }

static void parse_dc_time(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; update_time(&dc->when, line); }

static void parse_dc_watertemp(char *line, struct membuffer *str, void *_dc)
{ UNUSED(str); struct divecomputer *dc = _dc; dc->watertemp = get_temperature(line); }


int get_divemode(const char *divemodestring) {
	for (int i = 0; i < NUM_DIVEMODE; i++) {
		if (!strcmp(divemodestring, divemode_text[i]))
			return i;
	}
	return 0;
}

static void parse_event_keyvalue(void *_event, const char *key, const char *value)
{
	struct event *event = _event;
	int val = atoi(value);

	if (!strcmp(key, "type")) {
		event->type = val;
	} else if (!strcmp(key, "flags")) {
		event->flags = val;
	} else if (!strcmp(key, "value")) {
		event->value = val;
	} else if (!strcmp(key, "name")) {
		/* We get the name from the string handling */
	} else if (!strcmp(key,"divemode")) {
		event->value = get_divemode(value);
	} else if (!strcmp(key, "cylinder")) {
		/* NOTE! We add one here as a marker that "yes, we got a cylinder index" */
		event->gas.index = 1+get_index(value);
	} else if (!strcmp(key, "o2")) {
		event->gas.mix.o2 = get_fraction(value);
	} else if (!strcmp(key, "he")) {
		event->gas.mix.he = get_fraction(value);
	} else
		report_error("Unexpected event key/value pair (%s/%s)", key, value);
}

/* keyvalue "key" "value"
 * so we have two strings (possibly empty) in the membuffer, separated by a '\0' */
static void parse_dc_keyvalue(char *line, struct membuffer *str, void *_dc)
{
	const char *key, *value;
	struct divecomputer *dc = _dc;

	// Let's make sure we have two strings...
	int string_counter = 0;
	while(*line) {
		if (*line == '"')
			string_counter++;
		line++;
	}
	if (string_counter != 2)
		return;

	// stupidly the second string in the membuffer isn't NUL terminated;
	// asking for a cstring fixes that; interestingly enough, given that there are two
	// strings in the mb, the next command at the same time assigns a pointer to the
	// first string to 'key' and NUL terminates the second string (which then goes to 'value')
	key = mb_cstring(str);
	value = key + strlen(key) + 1;
	add_extra_data(dc, key, value);
}

static void parse_dc_event(char *line, struct membuffer *str, void *_dc)
{
	int m, s = 0;
	const char *name;
	struct divecomputer *dc = _dc;
	struct event event = { 0 }, *ev;

	m = strtol(line, &line, 10);
	if (*line == ':')
		s = strtol(line+1, &line, 10);
	event.time.seconds = m*60+s;

	for (;;) {
		char c;
		while (isspace(c = *line))
			line++;
		if (!c)
			break;
		line = parse_keyvalue_entry(parse_event_keyvalue, &event, line);
	}

	name = "";
	if (str->len)
		name = mb_cstring(str);
	ev = add_event(dc, event.time.seconds, event.type, event.flags, event.value, name);

	/*
	 * Older logs might mark the dive to be CCR by having an "SP change" event at time 0:00.
	 * Better to mark them being CCR on import so no need for special treatments elsewhere on
	 * the code.
	 */
	if (ev && event.time.seconds == 0 && event.type == SAMPLE_EVENT_PO2 && event.value && dc->divemode==OC) {
		dc->divemode = CCR;
	}

	if (ev && event_is_gaschange(ev)) {
		/*
		 * We subtract one here because "0" is "no index",
		 * and the parsing will add one for actual cylinder
		 * index data (see parse_event_keyvalue)
		 */
		ev->gas.index = event.gas.index-1;
		if (event.gas.mix.o2.permille || event.gas.mix.he.permille)
			ev->gas.mix = event.gas.mix;
	}
}

/* Not needed anymore - trip date calculated implicitly from first dive */
static void parse_trip_date(char *line, struct membuffer *str, void *trip)
{ UNUSED(line); UNUSED(str); UNUSED(trip); }

/* Not needed anymore - trip date calculated implicitly from first dive */
static void parse_trip_time(char *line, struct membuffer *str, void *trip)
{ UNUSED(line); UNUSED(str); UNUSED(trip); }

static void parse_trip_location(char *line, struct membuffer *str, void *_trip)
{ UNUSED(line); dive_trip_t *trip = _trip; trip->location = get_utf8(str); }

static void parse_trip_notes(char *line, struct membuffer *str, void *_trip)
{ UNUSED(line); dive_trip_t *trip = _trip; trip->notes = get_utf8(str); }

static void parse_settings_autogroup(char *line, struct membuffer *str, void *_unused)
{
	UNUSED(line);
	UNUSED(str);
	UNUSED(_unused);
	set_autogroup(true);
}

static void parse_settings_units(char *line, struct membuffer *str, void *unused)
{
	UNUSED(str);
	UNUSED(unused);
	if (line)
		set_informational_units(line);
}

static void parse_settings_userid(char *line, struct membuffer *str, void *_unused)
/* Keep this despite removal of the webservice as there are legacy logbook around
 * that still have this defined.
 */
{
	UNUSED(str);
	UNUSED(_unused);
	UNUSED(line);
}

static void parse_settings_prefs(char *line, struct membuffer *str, void *unused)
{
	UNUSED(str);
	UNUSED(unused);
	if (line)
		set_git_prefs(line);
}

/*
 * Our versioning is a joke right now, but this is more of an example of what we
 * *can* do some day. And if we do change the version, this warning will show if
 * you read with a version of subsurface that doesn't know about it.
 * We MUST keep this in sync with the XML version (so we can report a consistent
 * minimum datafile version)
 */
static void parse_settings_version(char *line, struct membuffer *str, void *_unused)
{
	UNUSED(str);
	UNUSED(_unused);
	int version = atoi(line);
	report_datafile_version(version);
	if (version > DATAFORMAT_VERSION)
		report_error("Git save file version %d is newer than version %d I know about", version, DATAFORMAT_VERSION);
}

/* The string in the membuffer is the version string of subsurface that saved things, just FYI */
static void parse_settings_subsurface(char *line, struct membuffer *str, void *_unused)
{
	UNUSED(line);
	UNUSED(str);
	UNUSED(_unused);
}

struct divecomputerid {
	const char *model;
	const char *nickname;
	const char *firmware;
	const char *serial;
	const char *cstr;
	unsigned int deviceid;
};

static void parse_divecomputerid_keyvalue(void *_cid, const char *key, const char *value)
{
	struct divecomputerid *cid = _cid;

	if (*value == '"') {
		value = cid->cstr;
		cid->cstr += strlen(cid->cstr)+1;
	}
	if (!strcmp(key, "deviceid")) {
		cid->deviceid = get_hex(value);
		return;
	}
	if (!strcmp(key, "serial")) {
		cid->serial = value;
		return;
	}
	if (!strcmp(key, "firmware")) {
		cid->firmware = value;
		return;
	}
	if (!strcmp(key, "nickname")) {
		cid->nickname = value;
		return;
	}
	report_error("Unknown divecomputerid key/value pair (%s/%s)", key, value);
}

/*
 * The 'divecomputerid' is a bit harder to parse than some other things, because
 * it can have multiple strings (but see the tag parsing for another example of
 * that) in addition to the non-string entries.
 *
 * We keep the "next" string in "id.cstr" and update it as we use it.
 */
static void parse_settings_divecomputerid(char *line, struct membuffer *str, void *_unused)
{
	UNUSED(_unused);
	struct divecomputerid id = { mb_cstring(str) };

	id.cstr = id.model + strlen(id.model) + 1;

	/* Skip the '"' that stood for the model string */
	line++;

	for (;;) {
		char c;
		while (isspace(c = *line))
			line++;
		if (!c)
			break;
		line = parse_keyvalue_entry(parse_divecomputerid_keyvalue, &id, line);
	}
	create_device_node(id.model, id.deviceid, id.serial, id.firmware, id.nickname);
}

static void parse_picture_filename(char *line, struct membuffer *str, void *_pic)
{
	UNUSED(line);
	struct picture *pic = _pic;
	pic->filename = get_utf8(str);
}

static void parse_picture_gps(char *line, struct membuffer *str, void *_pic)
{
	UNUSED(str);
	struct picture *pic = _pic;

	parse_location(line, &pic->location);
}

static void parse_picture_hash(char *line, struct membuffer *str, void *_pic)
{
	// we no longer use hashes to identify pictures, but we shouldn't
	// remove this parser or otherwise users get an ugly red warning when
	// opening old git repos
	UNUSED(line);
	UNUSED(_pic);
	UNUSED(str);
}

/* These need to be sorted! */
struct keyword_action dc_action[] = {
#undef D
#define D(x) { #x, parse_dc_ ## x }
	D(airtemp), D(date), D(dctype), D(deviceid), D(diveid), D(duration),
	D(event), D(keyvalue), D(lastmanualtime), D(maxdepth), D(meandepth), D(model), D(numberofoxygensensors),
	D(salinity), D(surfacepressure), D(surfacetime), D(time), D(watertemp)
};

/* Sample lines start with a space or a number */
static void divecomputer_parser(char *line, struct membuffer *str, void *_dc)
{
	char c = *line;
	if (c < 'a' || c > 'z')
		sample_parser(line, _dc);
	match_action(line, str, _dc, dc_action, ARRAY_SIZE(dc_action));
}

/* These need to be sorted! */
struct keyword_action dive_action[] = {
#undef D
#define D(x) { #x, parse_dive_ ## x }
	D(airtemp), D(buddy), D(cylinder), D(divemaster), D(divesiteid), D(duration),
	D(gps), D(location), D(notes), D(notrip), D(rating), D(suit),
	D(tags), D(visibility), D(watertemp), D(weightsystem)
};

static void dive_parser(char *line, struct membuffer *str, void *_dive)
{
	match_action(line, str, _dive, dive_action, ARRAY_SIZE(dive_action));
}

/* These need to be sorted! */
struct keyword_action site_action[] = {
#undef D
#define D(x) { #x, parse_site_ ## x }
	D(description), D(geo), D(gps), D(name), D(notes)
};

static void site_parser(char *line, struct membuffer *str, void *_ds)
{
	match_action(line, str, _ds, site_action, ARRAY_SIZE(site_action));
}

/* These need to be sorted! */
struct keyword_action trip_action[] = {
#undef D
#define D(x) { #x, parse_trip_ ## x }
	D(date), D(location), D(notes), D(time),
};

static void trip_parser(char *line, struct membuffer *str, void *_trip)
{
	match_action(line, str, _trip, trip_action, ARRAY_SIZE(trip_action));
}

/* These need to be sorted! */
static struct keyword_action settings_action[] = {
#undef D
#define D(x) { #x, parse_settings_ ## x }
	D(autogroup), D(divecomputerid), D(prefs), D(subsurface), D(units), D(userid), D(version)
};

static void settings_parser(char *line, struct membuffer *str, void *_unused)
{
	UNUSED(_unused);
	match_action(line, str, NULL, settings_action, ARRAY_SIZE(settings_action));
}

/* These need to be sorted! */
static struct keyword_action picture_action[] = {
#undef D
#define D(x) { #x, parse_picture_ ## x }
	D(filename), D(gps), D(hash)
};

static void picture_parser(char *line, struct membuffer *str, void *_pic)
{
	match_action(line, str, _pic, picture_action, ARRAY_SIZE(picture_action));
}

/*
 * We have a very simple line-based interface, with the small
 * complication that lines can have strings in the middle, and
 * a string can be multiple lines.
 *
 * The UTF-8 string escaping is *very* simple, though:
 *
 *  - a string starts and ends with double quotes (")
 *
 *  - inside the string we escape:
 *     (a) double quotes with '\"'
 *     (b) backslash (\) with '\\'
 *
 *  - additionally, for human readability, we escape
 *    newlines with '\n\t', with the exception that
 *    consecutive newlines are left unescaped (so an
 *    empty line doesn't become a line with just a tab
 *    on it).
 *
 * Also, while the UTF-8 string can have arbitrarily
 * long lines, the non-string parts of the lines are
 * never long, so we can use a small temporary buffer
 * on stack for that part.
 *
 * Also, note that if a line has one or more strings
 * in it:
 *
 *  - each string will be represented as a single '"'
 *    character in the output.
 *
 *  - all string will exist in the same 'membuffer',
 *    separated by NUL characters (that cannot exist
 *    in a string, not even quoted).
 */
static const char *parse_one_string(const char *buf, const char *end, struct membuffer *b)
{
	const char *p = buf;

	/*
	 * We turn multiple strings one one line (think dive tags) into one
	 * membuffer that has NUL characters in between strings.
	 */
	if (b->len)
		put_bytes(b, "", 1);

	while (p < end) {
		char replace;

		switch (*p++) {
		default:
			continue;
		case '\n':
			if (p < end && *p == '\t') {
				replace = '\n';
				break;
			}
			continue;
		case '\\':
			if (p < end) {
				replace = *p;
				break;
			}
			continue;
		case '"':
			replace = 0;
			break;
		}
		put_bytes(b, buf, p - buf - 1);
		if (!replace)
			break;
		put_bytes(b, &replace, 1);
		buf = ++p;
	}
	return p;
}

typedef void (line_fn_t)(char *, struct membuffer *, void *);
#define MAXLINE 500
static unsigned parse_one_line(const char *buf, unsigned size, line_fn_t *fn, void *fndata, struct membuffer *b)
{
	const char *end = buf + size;
	const char *p = buf;
	char line[MAXLINE+1];
	int off = 0;

	while (p < end) {
		char c = *p++;
		if (c == '\n')
			break;
		line[off] = c;
		off++;
		if (off > MAXLINE)
			off = MAXLINE;
		if (c == '"')
			p = parse_one_string(p, end, b);
	}
	line[off] = 0;
	fn(line, b, fndata);
	return p - buf;
}

/*
 * We keep on re-using the membuffer that we use for
 * strings, but the callback function can "steal" it by
 * saving its value and just clear the original.
 */
static void for_each_line(git_blob *blob, line_fn_t *fn, void *fndata)
{
	const char *content = git_blob_rawcontent(blob);
	unsigned int size = git_blob_rawsize(blob);
	struct membuffer str = { 0 };

	while (size) {
		unsigned int n = parse_one_line(content, size, fn, fndata, &str);
		content += n;
		size -= n;

		/* Re-use the allocation, but forget the data */
		str.len = 0;
	}
	free_buffer(&str);
}

#define GIT_WALK_OK   0
#define GIT_WALK_SKIP 1

static struct divecomputer *active_dc;
static struct dive *active_dive;
static dive_trip_t *active_trip;

static void finish_active_trip(void)
{
	dive_trip_t *trip = active_trip;

	if (trip) {
		active_trip = NULL;
		insert_trip(trip, &trip_table);
	}
}

static void finish_active_dive(void)
{
	struct dive *dive = active_dive;

	if (dive) {
		active_dive = NULL;
		record_dive(dive);
	}
}

static struct dive *create_new_dive(timestamp_t when)
{
	struct dive *dive = alloc_dive();

	/* We'll fill in more data from the dive file */
	dive->when = when;

	if (active_trip)
		add_dive_to_trip(dive, active_trip);
	return dive;
}

static bool validate_date(int yyyy, int mm, int dd)
{
	return yyyy > 1930 && yyyy < 3000 &&
		mm > 0 && mm < 13 &&
		dd > 0 && dd < 32;
}

static bool validate_time(int h, int m, int s)
{
	return h >= 0 && h < 24 &&
		m >= 0 && m < 60 &&
		s >=0 && s <= 60;
}

/*
 * Dive trip directory, name is 'nn-alphabetic[~hex]'
 */
static int dive_trip_directory(const char *root, const char *name)
{
	int yyyy = -1, mm = -1, dd = -1;

	if (sscanf(root, "%d/%d", &yyyy, &mm) != 2)
		return GIT_WALK_SKIP;
	dd = atoi(name);
	if (!validate_date(yyyy, mm, dd))
		return GIT_WALK_SKIP;
	finish_active_trip();
	active_trip = alloc_trip();
	return GIT_WALK_OK;
}

/*
 * Dive directory, name is [[yyyy-]mm-]nn-ddd-hh:mm:ss[~hex] in older git repositories
 * but [[yyyy-]mm-]nn-ddd-hh=mm=ss[~hex] in newer repos as ':' is an illegal character for Windows files
 * and 'timeoff' points to what should be the time part of
 * the name (the first digit of the hour).
 *
 * The root path will be of the form yyyy/mm[/tripdir],
 */
static int dive_directory(const char *root, const git_tree_entry *entry, const char *name, int timeoff)
{
	int yyyy = -1, mm = -1, dd = -1;
	int h, m, s;
	int mday_off, month_off, year_off;
	struct tm tm;

	/* Skip the '-' before the time */
	mday_off = timeoff;
	if (!mday_off || name[--mday_off] != '-')
		return GIT_WALK_SKIP;
	/* Skip the day name */
	while (mday_off > 0 && name[--mday_off] != '-')
		/* nothing */;

	mday_off = mday_off - 2;
	month_off = mday_off - 3;
	year_off = month_off - 5;
	if (mday_off < 0)
		return GIT_WALK_SKIP;

	/* Get the time of day -- parse both time formats so we can read old repos when not on Windows */
	if (sscanf(name+timeoff, "%d:%d:%d", &h, &m, &s) != 3 && sscanf(name+timeoff, "%d=%d=%d", &h, &m, &s) != 3)
		return GIT_WALK_SKIP;
	if (!validate_time(h, m, s))
		return GIT_WALK_SKIP;

	/*
	 * Using the "git_tree_walk()" interface is simple, but
	 * it kind of sucks as an interface because there is
	 * no sane way to pass the hierarchy to the callbacks.
	 * The "payload" is a fixed one-time thing: we'd like
	 * the "current trip" to be passed down to the dives
	 * that get parsed under that trip, but we can't.
	 *
	 * So "active_trip" is not the trip that is in the hierarchy
	 * _above_ us, it's just the trip that was _before_ us. But
	 * if a dive is not in a trip at all, we can't tell.
	 *
	 * We could just do a better walker that passes the
	 * return value around, but we hack around this by
	 * instead looking at the one hierarchical piece of
	 * data we have: the pathname to the current entry.
	 *
	 * This is pretty hacky. The magic '8' is the length
	 * of a pathname of the form 'yyyy/mm/'.
	 */
	if (strlen(root) == 8)
		finish_active_trip();

	/*
	 * Get the date. The day of the month is in the dive directory
	 * name, the year and month might be in the path leading up
	 * to it.
	 */
	dd = atoi(name + mday_off);
	if (year_off < 0) {
		if (sscanf(root, "%d/%d", &yyyy, &mm) != 2)
			return GIT_WALK_SKIP;
	} else
		yyyy = atoi(name + year_off);
	if (month_off >= 0)
		mm = atoi(name + month_off);

	if (!validate_date(yyyy, mm, dd))
		return GIT_WALK_SKIP;

	/* Ok, close enough. We've gotten sufficient information */
	memset(&tm, 0, sizeof(tm));
	tm.tm_hour = h;
	tm.tm_min = m;
	tm.tm_sec = s;
	tm.tm_year = yyyy;
	tm.tm_mon = mm-1;
	tm.tm_mday = dd;

	finish_active_dive();
	active_dive = create_new_dive(utc_mktime(&tm));
	memcpy(active_dive->git_id, git_tree_entry_id(entry)->id, 20);
	return GIT_WALK_OK;
}

static int picture_directory(const char *root, const char *name)
{
	UNUSED(root);
	UNUSED(name);
	if (!active_dive)
		return GIT_WALK_SKIP;
	return GIT_WALK_OK;
}

/*
 * Return the length of the string without the unique part.
 */
static int nonunique_length(const char *str)
{
	int len = 0;

	for (;;) {
		char c = *str++;
		if (!c || c == '~')
			return len;
		len++;
	}
}

/*
 * When hitting a directory node, we have a couple of cases:
 *
 *  - It's just a date entry - all numeric (either year or month):
 *
 *       [yyyy|mm]
 *
 *    We don't do anything with these, we just traverse into them.
 *    The numeric data will show up as part of the full path when
 *    we hit more interesting entries.
 *
 *  - It's a trip directory. The name will be of the form
 *
 *       nn-alphabetic[~hex]
 *
 *    where 'nn' is the day of the month (year and month will be
 *    encoded in the path leading up to this).
 *
 *  - It's a dive directory. The name will be of the form
 *
 *       [[yyyy-]mm-]nn-ddd-hh=mm=ss[~hex]
 *
 *    (older versions had this as [[yyyy-]mm-]nn-ddd-hh:mm:ss[~hex]
 *     but that faile on Windows)
 *
 *    which describes the date and time of a dive (yyyy and mm
 *    are optional, and may be encoded in the path leading up to
 *    the dive).
 *
 *  - It is a per-dive picture directory ("Pictures")
 *
 *  - It's some random non-dive-data directory.
 *
 *    If it doesn't match the above patterns, we'll ignore them
 *    for dive loading purposes, and not even recurse into them.
 */
static int walk_tree_directory(const char *root, const git_tree_entry *entry)
{
	const char *name = git_tree_entry_name(entry);
	int digits = 0, len;
	char c;

	if (!strcmp(name, "Pictures"))
		return picture_directory(root, name);

	if (!strcmp(name, "01-Divesites"))
		return GIT_WALK_OK;

	while (isdigit(c = name[digits]))
		digits++;

	/* Doesn't start with two or four digits? Skip */
	if (digits != 4 && digits != 2)
		return GIT_WALK_SKIP;

	/* Only digits? Do nothing, but recurse into it */
	if (!c)
		return GIT_WALK_OK;

	/* All valid cases need to have a slash following */
	if (c != '-')
		return GIT_WALK_SKIP;

	/* Do a quick check for a common dive case */
	len = nonunique_length(name);

	/*
	 * We know the len is at least 3, because we had at least
	 * two digits and a dash
	 */
	if (name[len-3] == ':' || name[len-3] == '=')
		return dive_directory(root, entry, name, len-8);

	if (digits != 2)
		return GIT_WALK_SKIP;

	return dive_trip_directory(root, name);
}

git_blob *git_tree_entry_blob(git_repository *repo, const git_tree_entry *entry)
{
	const git_oid *id = git_tree_entry_id(entry);
	git_blob *blob;

	if (git_blob_lookup(&blob, repo, id))
		return NULL;
	return blob;
}

static struct divecomputer *create_new_dc(struct dive *dive)
{
	struct divecomputer *dc = &dive->dc;

	while (dc->next)
		dc = dc->next;
	/* Did we already fill that in? */
	if (dc->samples || dc->model || dc->when) {
		struct divecomputer *newdc = calloc(1, sizeof(*newdc));
		if (!newdc)
			return NULL;
		dc->next = newdc;
		dc = newdc;
	}
	dc->when = dive->when;
	dc->duration = dive->duration;
	return dc;
}

/*
 * We should *really* try to delay the dive computer data parsing
 * until necessary, in order to reduce load-time. The parsing is
 * cheap, but the loading of the git blob into memory can be pretty
 * costly.
 */
static int parse_divecomputer_entry(git_repository *repo, const git_tree_entry *entry, const char *suffix)
{
	UNUSED(suffix);
	git_blob *blob = git_tree_entry_blob(repo, entry);

	if (!blob)
		return report_error("Unable to read divecomputer file");

	active_dc = create_new_dc(active_dive);
	for_each_line(blob, divecomputer_parser, active_dc);
	git_blob_free(blob);
	active_dc = NULL;
	return 0;
}

/*
 * NOTE! The "git_id" for the dive is the hash for the whole dive directory.
 * As such, it covers not just the dive, but the divecomputers and the
 * pictures too. So if any of the dive computers change, the dive cache
 * has to be invalidated too.
 */
static int parse_dive_entry(git_repository *repo, const git_tree_entry *entry, const char *suffix)
{
	struct dive *dive = active_dive;
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read dive file");
	if (*suffix)
		dive->number = atoi(suffix+1);
	cylinder_index = weightsystem_index = 0;
	o2pressure_sensor = 1;
	for_each_line(blob, dive_parser, active_dive);
	git_blob_free(blob);
	return 0;
}

static int parse_site_entry(git_repository *repo, const git_tree_entry *entry, const char *suffix)
{
	if (*suffix == '\0')
		return report_error("Dive site without uuid");
	uint32_t uuid = strtoul(suffix, NULL, 16);
	struct dive_site *ds = alloc_or_get_dive_site(uuid, &dive_site_table);
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read dive site file");
	for_each_line(blob, site_parser, ds);
	git_blob_free(blob);
	return 0;
}

static int parse_trip_entry(git_repository *repo, const git_tree_entry *entry)
{
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read trip file");
	for_each_line(blob, trip_parser, active_trip);
	git_blob_free(blob);
	return 0;
}

static int parse_settings_entry(git_repository *repo, const git_tree_entry *entry)
{
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read settings file");
	for_each_line(blob, settings_parser, NULL);
	git_blob_free(blob);
	return 0;
}

static int parse_picture_entry(git_repository *repo, const git_tree_entry *entry, const char *name)
{
	git_blob *blob;
	struct picture *pic;
	int hh, mm, ss, offset;
	char sign;

	/*
	 * The format of the picture name files is just the offset within
	 * the dive in form [[+-]hh=mm=ss (previously [[+-]hh:mm:ss, but
	 * that didn't work on Windows), possibly followed by a hash to
	 * make the filename unique (which we can just ignore).
	 */
	if (sscanf(name, "%c%d:%d:%d", &sign, &hh, &mm, &ss) != 4 &&
	    sscanf(name, "%c%d=%d=%d", &sign, &hh, &mm, &ss) != 4)
		return report_error("Unknown file name %s", name);
	offset = ss + 60*(mm + 60*hh);
	if (sign == '-')
		offset = -offset;

	blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read picture file");

	pic = alloc_picture();
	pic->offset.seconds = offset;

	for_each_line(blob, picture_parser, pic);
	dive_add_picture(active_dive, pic);
	git_blob_free(blob);
	return 0;
}

static int walk_tree_file(const char *root, const git_tree_entry *entry, git_repository *repo)
{
	struct dive *dive = active_dive;
	dive_trip_t *trip = active_trip;
	const char *name = git_tree_entry_name(entry);
	if (verbose > 1)
		fprintf(stderr, "git load handling file %s\n", name);
	switch (*name) {
	case '-': case '+':
		if (dive)
			return parse_picture_entry(repo, entry, name);
		break;
	case 'D':
		if (dive && !strncmp(name, "Divecomputer", 12))
			return parse_divecomputer_entry(repo, entry, name+12);
		if (dive && !strncmp(name, "Dive", 4))
			return parse_dive_entry(repo, entry, name+4);
		break;
	case 'S':
		if (!strncmp(name, "Site", 4))
			return parse_site_entry(repo, entry, name + 5);
		break;
	case '0':
		if (trip && !strcmp(name, "00-Trip"))
			return parse_trip_entry(repo, entry);
		if (!strcmp(name, "00-Subsurface"))
			return parse_settings_entry(repo, entry);
		break;
	}
	report_error("Unknown file %s%s (%p %p)", root, name, dive, trip);
	return GIT_WALK_SKIP;
}

static int walk_tree_cb(const char *root, const git_tree_entry *entry, void *payload)
{
	git_repository *repo = payload;
	git_filemode_t mode = git_tree_entry_filemode(entry);

	if (mode == GIT_FILEMODE_TREE)
		return walk_tree_directory(root, entry);

	walk_tree_file(root, entry, repo);
	/* Ignore failed blob loads */
	return GIT_WALK_OK;
}

static int load_dives_from_tree(git_repository *repo, git_tree *tree)
{
	git_tree_walk(tree, GIT_TREEWALK_PRE, walk_tree_cb, repo);
	return 0;
}

void clear_git_id(void)
{
	saved_git_id = NULL;
}

void set_git_id(const struct git_oid * id)
{
	static char git_id_buffer[GIT_OID_HEXSZ+1];

	git_oid_tostr(git_id_buffer, sizeof(git_id_buffer), id);
	saved_git_id = git_id_buffer;
}

static int find_commit(git_repository *repo, const char *branch, git_commit **commit_p)
{
	git_object *object;

	if (git_revparse_single(&object, repo, branch))
		return report_error("Unable to look up revision '%s'", branch);
	if (git_object_peel((git_object **)commit_p, object, GIT_OBJ_COMMIT))
		return report_error("Revision '%s' is not a valid commit", branch);
	return 0;
}

static int do_git_load(git_repository *repo, const char *branch)
{
	int ret;
	git_commit *commit;
	git_tree *tree;

	ret = find_commit(repo, branch, &commit);
	if (ret)
		return ret;
	if (git_commit_tree(&tree, commit))
		return report_error("Could not look up tree of commit in branch '%s'", branch);
	git_storage_update_progress(translate("gettextFromC", "Load dives from local cache"));
	ret = load_dives_from_tree(repo, tree);
	if (!ret) {
		set_git_id(git_commit_id(commit));
		git_storage_update_progress(translate("gettextFromC", "Successfully opened dive data"));
	}
	git_object_free((git_object *)tree);

	return ret;
}

const char *get_sha(git_repository *repo, const char *branch)
{
	static char git_id_buffer[GIT_OID_HEXSZ+1];
	git_commit *commit;
	if (find_commit(repo, branch, &commit))
		return NULL;
	git_oid_tostr(git_id_buffer, sizeof(git_id_buffer), (const git_oid *)commit);
	return git_id_buffer;
}

/*
 * Like git_save_dives(), this silently returns a negative
 * value if it's not a git repository at all (so that you
 * can try to load it some other way.
 *
 * If it is a git repository, we return zero for success,
 * or report an error and return 1 if the load failed.
 */
int git_load_dives(struct git_repository *repo, const char *branch)
{
	int ret;

	if (repo == dummy_git_repository)
		return report_error("Unable to open git repository at '%s'", branch);
	ret = do_git_load(repo, branch);
	git_repository_free(repo);
	free((void *)branch);
	finish_active_dive();
	finish_active_trip();
	return ret;
}
