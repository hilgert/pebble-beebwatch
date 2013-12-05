// Beebwatch, for Pebble
// by Tom Gidden <tom@gidden.net>
//
// Written: May 4, 2013
// Updated to Pebble OS 2: December 2013
//
// This code is hacked together from a number of sources, most notably the
// Pebble SDK examples.
//
// The code's a bit of a mess, but once the SDK settles down and is a bit
// better documented, I'll probably rework this code to match. Until then,
// my apologies: my code's not usually this messy.
//
// Copyright on the BBC Clock face design is probably held by the BBC,
// although I've redrawn it from scratch. Copyright on the fonts is held
// by me (Tom Gidden), but is based on the characters generated by the
// Mullard SAA5050 chip as used in Teletext.

#include "pebble.h"

#define NO 0
#define YES 1

// Boolean preferences:
enum Settings {
    SETTING_SECHAND = 1,
    SETTING_SHOWTIME = 2,
    SETTING_SHOWDATE = 3,
    SETTING_IS24H = 4
};

uint8_t sechand = NO;
uint8_t showtime = YES;
uint8_t showdate = YES;
uint8_t is_24h = YES;

uint8_t update_on_next_tick = YES;

// Syncing of configuration settings with PebbleJS
static AppSync app;
static uint8_t buffer[256];

// The main window itself
static Window *window;
static Layer *window_layer;

// watchface is the background image
static GBitmap *watchface_image;
static BitmapLayer *watchface_layer;

// The frame of the watchface is unsurprisingly pivotal (no pun intended),
// and is used in all of the graphics update routines, so it's best to
// just store it rather than recalculating it each time. In addition, the
// middle point of the frame is also used, so we store it too: relative to
// the frame.
static GPoint watchface_center;
static GRect watchface_frame;

// centerdot is to cover up the where the hands cross the center, which is
// unsightly. Previous versions used a rounded rectangle or a circle, but
// I think this might be more efficient...
static GBitmap *centerdot_image;
static BitmapLayer *centerdot_layer;
static GRect centerdot_frame;

// Hour and minute hands. Was previously done with rotating bitmaps, but
// the current 2.0 BETA SDK is incomplete here.
static GPath *minute_path;
static GPath *hour_path;
static Layer *hmhands_layer;

// The second hand, however, can be drawn with a simple line, as in this
// design it's not meant to be thick.
static Layer *sechand_layer;

// Text layers and fonts for digital time and date. If these are not
// enabled, they just don't get loaded or used, so no big deal.
static TextLayer *date_layer;
static GFont small_font;
#define FONT_SMALL RESOURCE_ID_FONT_MALLARD_16

static TextLayer *time_layer;
static GFont large_font;
#define FONT_LARGE RESOURCE_ID_FONT_MALLARD_CONDENSED_32

// Text buffers for the digital readout.
static char date_text[] = "Xxx 00 Xxx";
static char *time_format;
static char time_text[] = "00:00:00";

// Store the time from the event, so we can use it in later
// functions. Since we're not building a world-clock, or handling General
// Relativity, I think it's safe to treat time as a global variable...
static struct tm *pebble_time;

// Simple rectangle paths for the hands
static const GPathInfo BIG_HOUR_HAND = {
  4, (GPoint []){
    {-4, -49},
    {4, -49},
    {4, 0},
    {-4, 0}
  }
};
static const GPathInfo SMALL_HOUR_HAND = {
  4, (GPoint []){
    {-3, -37},
    {4, -37},
    {4, 0},
    {-3, 0}
  }
};
static const GPathInfo BIG_MINUTE_HAND = {
  4, (GPoint []){
    {-3, -72},
    {3, -72},
    {3, 0},
    {-3, 0}
  }
};
static const GPathInfo SMALL_MINUTE_HAND = {
  4, (GPoint []){
    {-2, -56},
    {3, -56},
    {3, 0},
    {-2, 0}
  }
};

static inline int big()
{
    return !(showtime || showdate);
}

static void load_image_to_bitmap_layer(GBitmap **image, BitmapLayer **layer, GRect *frame, const int resource_id)
// Loads a resource into an image, layer and frame.
{
    // If the vars are already set, then we'll need to delete them at the
    // end of this routine to prevent a leak.
    GBitmap *old_image = *image;
    BitmapLayer *old_layer = *layer;

    // Load the image
    *image = gbitmap_create_with_resource(resource_id);

    // Get the size and store it.
    frame->size = (*image)->bounds.size;

    // Create a new bitmap layer
    *layer = bitmap_layer_create(*frame);

    // And load the image into the layer.
    bitmap_layer_set_bitmap(*layer, *image);

    // If it previously existed, we can kill the old data now.
    if(old_image) gbitmap_destroy(old_image);
    if(old_layer) bitmap_layer_destroy(old_layer);
}

static void hmhands_update_proc(Layer *layer, GContext *ctx)
// On Pebble OS 1, the polygon drawing routine was fairly flaky, so the
// hands were done with bitmap rotations.  On Pebble OS 2 BETA, bitmap
// rotations seem incomplete (or at least, undocumented), so it's back to
// polygons. Fortunately, they work a lot better now.
{
    if(!pebble_time) return;

    // We're not going to bother stroking the polygon: just fill
    graphics_context_set_fill_color(ctx, GColorWhite);

    // Rotate and draw minute hand
    gpath_rotate_to(minute_path, TRIG_MAX_ANGLE * pebble_time->tm_min / 60);
    gpath_draw_filled(ctx, minute_path);

    // Rotate and draw hour hand
    gpath_rotate_to(hour_path, (TRIG_MAX_ANGLE * (((pebble_time->tm_hour % 12) * 6) + (pebble_time->tm_min / 10))) / (12 * 6));
    gpath_draw_filled(ctx, hour_path);
}

static void sechand_update_proc(Layer *layer, GContext *ctx)
// The second-hand is drawn as a simple line.
{
    if(!pebble_time) return;

    // The second-hand has a "counterbalance", so we actually start
    // the other side of the center.
    static GPoint endpoint1, endpoint2;

    graphics_context_set_stroke_color(ctx, GColorWhite);

    // We could probably precalc these calculations, but screw it.  The
    // length of the second-hand is the same as the radius of the
    // watchface, which is the same as watchface_center.x (as that's half
    // the width a.k.a. diameter)
    int32_t a = TRIG_MAX_ANGLE * pebble_time->tm_sec / 60;
    int16_t dy = (int16_t)(-cos_lookup(a) * (int32_t)watchface_center.x / TRIG_MAX_RATIO);
    int16_t dx = (int16_t)(sin_lookup(a) * (int32_t)watchface_center.x / TRIG_MAX_RATIO);

    // Draw a line _across_ the center to the edge of the face.
    endpoint1.x = watchface_center.x + dx;
    endpoint1.y = watchface_center.y + dy;
    endpoint2.x = watchface_center.x - dx/3;
    endpoint2.y = watchface_center.y - dy/3;

    graphics_draw_line(ctx, endpoint1, endpoint2);
}

static void clear_ui()
// Pebble has little to no memory management, so we have to clear up. This might
// be a bit excessive... in particular, is it necessary to remove a layer before
// destroying it?
//
// Regardless, this shouldn't harm and gives setup_ui() a clean run at it.
{
    if(hmhands_layer) {
        layer_remove_from_parent(hmhands_layer);
        layer_destroy(hmhands_layer);
        hmhands_layer = NULL;
    }

    if(hour_path) {
        gpath_destroy(hour_path);
        hour_path = NULL;
    }

    if(sechand_layer) {
        layer_remove_from_parent(sechand_layer);
        layer_destroy(sechand_layer);
        sechand_layer = NULL;
    }

    if(minute_path) {
        gpath_destroy(minute_path);
        minute_path = NULL;
    }

    if(centerdot_layer) {
        layer_remove_from_parent(bitmap_layer_get_layer(centerdot_layer));
        bitmap_layer_destroy(centerdot_layer);
        centerdot_layer = NULL;
    }

    if(centerdot_image) {
        gbitmap_destroy(centerdot_image);
        centerdot_image = NULL;
    }

    if(watchface_layer) {
        layer_remove_from_parent(bitmap_layer_get_layer(watchface_layer));
        bitmap_layer_destroy(watchface_layer);
        watchface_layer = NULL;
    }

    if(watchface_image) {
        gbitmap_destroy(watchface_image);
        watchface_image = NULL;
    }

    if(date_layer) {
        layer_remove_from_parent(text_layer_get_layer(date_layer));
        text_layer_destroy(date_layer);
        date_layer = NULL;
    }

    if(time_layer) {
        layer_remove_from_parent(text_layer_get_layer(time_layer));
        text_layer_destroy(time_layer);
        time_layer = NULL;
    }

    if(large_font) {
        fonts_unload_custom_font(large_font);
        large_font = NULL;
    }

    if(small_font) {
        fonts_unload_custom_font(small_font);
        small_font = NULL;
    }
}

static void setup_ui()
// Load the images, fonts and paths for the UI elements, either big or
// small.
{
    // If this routine has been run before, we clear the old paths. This
    // will allow dynamic config of watch size.
    clear_ui();

    // If there's a digital readout, then we initialise the string format
    // here. As mentioned above, this might need to be moved into
    // something in the main loop if it becomes possible to change format
    // mid-run.
    if (showtime) {
        if (is_24h) {
            if (sechand)
                time_format = "%R:%S";
            else
                time_format = "%R";
        }
        else {
            if (sechand)
                time_format = "%I:%M:%S";
            else
                time_format = "%I:%M";
        }
    }

    // Not unexpectedly, most coordinates for the face, hands, etc. are
    // relative to the watchface size and position.
    watchface_frame = (GRect) {
        .origin = { .x = 0, .y = 12 }
    };

    if(big()) {
        // just analogue: stretches the full width of the watch, and is
        // vertically centered.
        watchface_frame.origin.x = 0;
        watchface_frame.origin.y = 12;
    }
    else {
        // A mix of analogue and digital. The watch face is narrower, and
        // so needs to be horizontally centered. Vertical positioning is
        // determined by whether or not there's a digital readout of the
        // time.
        watchface_frame.origin.x = 16;
        if(showtime)
            watchface_frame.origin.y = 0;
        else
            watchface_frame.origin.y = 14;
    }

    // Load the watchface first
    load_image_to_bitmap_layer(&watchface_image, &watchface_layer, &watchface_frame, big() ? RESOURCE_ID_IMAGE_BIG_WATCHFACE : RESOURCE_ID_IMAGE_SMALL_WATCHFACE);

    // The center of the watchface (relative to the origin of the frame)
    // is used in laying out the hands.
    watchface_center = GPoint(watchface_frame.size.w/2, watchface_frame.size.h/2);

    // Load and position the hour hand
    hour_path = gpath_create(big() ? &BIG_HOUR_HAND : &SMALL_HOUR_HAND);
    gpath_move_to(hour_path, watchface_center);

    // Load and position the minute hand
    minute_path = gpath_create(big() ? &BIG_MINUTE_HAND : &SMALL_MINUTE_HAND);
    gpath_move_to(minute_path, watchface_center);

    // Load the center dot
    load_image_to_bitmap_layer(&centerdot_image, &centerdot_layer, &centerdot_frame, big() ? RESOURCE_ID_IMAGE_BIG_CENTERDOT : RESOURCE_ID_IMAGE_SMALL_CENTERDOT);

    // Add the background layer to the window.
    layer_add_child(window_layer, bitmap_layer_get_layer(watchface_layer));

    // Hands: To make updates easier (as hours and minutes are always
    // updated at the same time due to slew on the hour hand through the
    // hour), the hour- and minute-hand are separate image sublayers of a
    // generic layer for both hands. This means they share the same update
    // routine.
    hmhands_layer = layer_create(watchface_frame);
    layer_set_update_proc(hmhands_layer, hmhands_update_proc);

    // Add the combined hands layer to the window
    layer_add_child(window_layer, hmhands_layer);

    // Second-hand, if there is one:
    if(sechand) {
        sechand_layer = layer_create(watchface_frame);
        layer_set_update_proc(sechand_layer, sechand_update_proc);
        layer_add_child(window_layer, sechand_layer);
    }

    // Load bitmap for center dot background
    // Center-align it on the watchface center
    centerdot_frame.origin.x = watchface_frame.origin.x + watchface_center.x - centerdot_frame.size.w / 2;
    centerdot_frame.origin.y = watchface_frame.origin.y + watchface_center.y - centerdot_frame.size.h / 2;
    layer_set_frame(bitmap_layer_get_layer(centerdot_layer), centerdot_frame);

    // Add it to the window.
    layer_add_child(window_layer, bitmap_layer_get_layer(centerdot_layer));

    if(showdate) {
        // Date. This is intentionally formatted the same way Teletext
        // formatted it: Day Date Mon, with Date padded with spaces.
        date_layer = text_layer_create(GRect(13, 146, 144-13, 20));
        text_layer_set_text_color(date_layer, GColorWhite);
        text_layer_set_background_color(date_layer, GColorClear);
        text_layer_set_text_alignment(date_layer, GTextAlignmentLeft);

        if(!small_font)
            small_font = fonts_load_custom_font(resource_get_handle(FONT_SMALL));

        text_layer_set_font(date_layer, small_font);

        layer_add_child(window_layer, text_layer_get_layer(date_layer));
    }

    // If there's a digital time readout, then set up a layer to print
    // the time:
    if(showtime) {
        // If there's no seconds, then the bounding box is
        // shifted. However, this does mess up the nice neat
        // Teletext-style monospaced layout. Oh well.
        if(sechand)
            time_layer = text_layer_create(GRect(25, 112, 144-25, 33));
        else
            time_layer = text_layer_create(GRect(43, 112, 144-43, 33));

        text_layer_set_text_color(time_layer, GColorWhite);
        text_layer_set_background_color(time_layer, GColorClear);
        text_layer_set_text_alignment(time_layer, GTextAlignmentLeft);

        if(!large_font)
            large_font = fonts_load_custom_font(resource_get_handle(FONT_LARGE));

        text_layer_set_font(time_layer, large_font);

        layer_add_child(window_layer, text_layer_get_layer(time_layer));
    }
}

static void tick(struct tm *t, TimeUnits units_changed)
{
    // If we were passed an event, then we should already have the current
    // time. If not, then we're probably being called from handle_init, so
    // we need to get it.
    if(t)
        pebble_time = t;
    else {
        time_t now = time(NULL);
        pebble_time = localtime(&now);
    }

    // If the settings have been changed, or they haven't been initialised
    // yet, then we run the UI setup routine before any drawing takes
    // place.
    if(update_on_next_tick) {
        setup_ui();
        update_on_next_tick = NO;
    }

    // If digital readout is on, then print the time:
    if(showtime) {
        // Format the time with the chosen time format
        strftime(time_text, sizeof(time_text), time_format, pebble_time);

        // Replace the leading zero with blank if we're in 12h mode.
        if ((!is_24h) && (time_text[0] == '0')) {
            time_text[0] = ' ';
        }

        // And set it for display. This should automatically mark the
        // layer as dirty.
        text_layer_set_text(time_layer, time_text);
    }

    // If we're displaying the date, update the date string
    // every hour, and also on initialisation.
    if(showdate) {
        if(date_text[0]=='X' || (pebble_time->tm_sec == 0 && pebble_time->tm_min == 0)) {
            strftime(date_text, sizeof(date_text), "%a %e %b", pebble_time);
            text_layer_set_text(date_layer, date_text);
        }
    }

    // If we're displaying a second-hand, then mark the second-hand layer
    // as dirty for redrawing.
    if(sechand)
        layer_mark_dirty(sechand_layer);

    // Update the hour/minute hands, if:
    //
    //   a) !t, in which case, we're being called from handle_init, so
    //      need to do the initial update;
    //
    //   b) Current seconds = 0 (ie. we're on the minute); or
    //
    //   c) Display of seconds is not enabled, which implies this event
    //      handler is running every minute anyway.
    if (!sechand || (pebble_time->tm_sec == 0)) {
        layer_mark_dirty(hmhands_layer);
    }
}

static void tuple_changed_callback(const uint32_t key, const Tuple* tuple_new, const Tuple* tuple_old, void* context)
// Configuration data from PebbleJS has been received.
{
    uint8_t value = tuple_new->value->uint8;

    switch (key) {
    case SETTING_SECHAND:
        sechand = value ? YES : NO;
        update_on_next_tick = YES;
        break;

    case SETTING_SHOWTIME:
        showtime = value ? YES : NO;
        update_on_next_tick = YES;
        break;

    case SETTING_SHOWDATE:
        showdate = value ? YES : NO;
        if(showdate) date_text[0] = 'X'; // Dirty the date so it gets refreshed on next tick
        update_on_next_tick = YES;
        break;

    case SETTING_IS24H:
        is_24h = value ? YES : NO;
        update_on_next_tick = YES;
        break;
    }

    // If the settings were updated, then we need to schedule the next
    // tick.  Since it might change from every-minute to every-second (or
    // vice versa), we need to do a full resubscribe.
    if(update_on_next_tick) {

        // Write the value to persistent storage
        persist_write_int(key, value);

        // (Re-)schedule the timer
        tick_timer_service_unsubscribe();
        tick_timer_service_subscribe(sechand ? SECOND_UNIT : MINUTE_UNIT, tick);
    }
}

static void app_error_callback(DictionaryResult dict_error, AppMessageResult app_message_error, void* context)
// Error from... um... the thing.
{
    APP_LOG(APP_LOG_LEVEL_DEBUG, "app error %d", app_message_error);
}

static void window_load(Window *_window)
// Window load event.
{
    // Initialise the window variables
    window = _window;
    window_layer = window_get_root_layer(window);

    // And schedule a reinitialise on the next tick
    update_on_next_tick = YES;

    // Call the tick handler once to initialise the face
    tick(NULL, sechand ? SECOND_UNIT : MINUTE_UNIT);
}

static void window_unload(Window *window)
// Window unload event
{
    // Stop the tick handler
    tick_timer_service_unsubscribe();

    // And deallocate everything
    clear_ui();
}

static void send_cfg_to_js(void)
// Send settings to PebbleJS so it can store them
{
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);

  if (iter == NULL) return;

  dict_write_uint8(iter, SETTING_SECHAND, sechand);
  dict_write_uint8(iter, SETTING_SHOWTIME, showtime);
  dict_write_uint8(iter, SETTING_SHOWDATE, showdate);
  dict_write_uint8(iter, SETTING_IS24H, is_24h);

  dict_write_end(iter);

  app_message_outbox_send();
}

static void init(void)
// Initialise the app
{
    // Initialise settings from persistent storage
    if(persist_exists(SETTING_SECHAND))
        sechand = persist_read_int(SETTING_SECHAND);

    if(persist_exists(SETTING_SHOWTIME))
        showtime = persist_read_int(SETTING_SHOWTIME);

    if(persist_exists(SETTING_SHOWDATE))
        showdate = persist_read_int(SETTING_SHOWDATE);

    if(persist_exists(SETTING_IS24H))
        is_24h = persist_read_int(SETTING_IS24H);
    else
        is_24h = clock_is_24h_style();

    // Create and initialise the main window
    window = window_create();
    window_set_background_color(window, GColorBlack);
    window_set_window_handlers(window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload,
    });

    if (window == NULL) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "OOM: couldn't allocate window");
        return;
    }

    app_message_open(160, 160);

    Tuplet tuples[] = {
        TupletInteger(SETTING_SECHAND, sechand),
        TupletInteger(SETTING_SHOWTIME, showtime),
        TupletInteger(SETTING_SHOWDATE, showdate),
        TupletInteger(SETTING_IS24H, is_24h)
    };

    app_sync_init(&app,
                  buffer, sizeof(buffer),
                  tuples, ARRAY_LENGTH(tuples),
                  tuple_changed_callback,
                  app_error_callback,
                  NULL);

    // And send the persistent config to PebbleJS
    send_cfg_to_js();

    // Load the window onto the UI stack
    window_stack_push(window, true);
}

static void deinit()
{
    // Shut down PebbleJS
    app_sync_deinit(&app);

    // And close the main window
    window_destroy(window);
}

int main(void)
{
    init();
    app_event_loop();
    deinit();
}
