#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_pwm.h>

#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>

#include <gui/gui.h>
#include <locale/locale.h>

#define TAG "laser_pointer"

typedef enum {
    AppEventTypeKey,
    // You can add additional events here.
} AppEventType;

typedef struct {
    AppEventType type; // The reason for this event.
    InputEvent input; // This data is specific to keypress data.
    // You can add additional data that is helpful for your events.
} AppEvent;

typedef struct {
    FuriString* buffer;
    // You can add additional state here.
    bool pressed;
    bool keep_on;
    bool short_expect_release;
} AppData;

typedef struct {
    FuriMessageQueue* queue; // Message queue (AppEvent items to process).
    FuriMutex* mutex; // Used to provide thread safe access to data.
    AppData* data; // Data accessed by multiple threads (acquire the mutex before accessing!)
} AppContext;

// Invoked when input (button press) is detected.  We queue a message and then return to the caller.
static void input_callback(InputEvent* input_event, FuriMessageQueue* queue) {
    furi_assert(queue);
    AppEvent event = {.type = AppEventTypeKey, .input = *input_event};
    furi_message_queue_put(queue, &event, FuriWaitForever);
}

// Invoked by the draw callback to render the screen. We render our UI on the callback thread.
static void render_callback(Canvas* canvas, void* ctx) {
    // Attempt to aquire context, so we can read the data.
    AppContext* app_context = ctx;
    if(furi_mutex_acquire(app_context->mutex, 200) != FuriStatusOk) {
        return;
    }

    AppData* data = app_context->data;
    // furi_string_printf(data->buffer, "Laser Pointer");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 15, 25, AlignLeft, AlignTop, "Laser Pointer"); // big title
    char* state = (data->pressed ? "ON" : "OFF");
    if(data->keep_on) {
        state = "ON (Keep ON)";
    }
    furi_string_printf(data->buffer, "State: %s", state);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 15, 40, AlignLeft, AlignTop, furi_string_get_cstr(data->buffer));

    // Release the context, so other threads can update the data.
    furi_mutex_release(app_context->mutex);
}
// https://github.com/jamisonderek/flipper-zero-tutorials/blob/efb1269e02d7b80c0b5961ed02072e73b5aca916/gpio/dac/app.c#L579
static bool attempt_set_5v_state(bool enable) {
    if(furi_hal_power_is_otg_enabled() == enable) {
        return true;
    }
    bool success = false;
    uint8_t attempts = 5;

    while(attempts-- > 0) {
        FURI_LOG_T(TAG, "Setting 5V state to %s", enable ? "ON" : "OFF");
        if(enable) {
            FURI_LOG_T(TAG, "Enabling 5V state");
            if(furi_hal_power_enable_otg()) {
                FURI_LOG_T(TAG, "OTG enabled");
                success = true;
                break;
            }
        } else {
            FURI_LOG_T(TAG, "Disabling 5V state");
            furi_hal_power_disable_otg();
        }
    }

    if(!success) {
        FURI_LOG_E(TAG, "Failed to set 5V state to %s", enable ? "ON" : "OFF");
    }

    return success;
}

void update_pointer_state(AppContext* app_context) {
    if(furi_mutex_acquire(app_context->mutex, FuriWaitForever) == FuriStatusOk) {
        attempt_set_5v_state(app_context->data->pressed || app_context->data->keep_on);
        furi_mutex_release(app_context->mutex);
    }
}

int32_t laser_pointer_app(void* p) {
    UNUSED(p);

    // Configure our initial data.
    AppContext* app_context = malloc(sizeof(AppContext));
    app_context->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app_context->data = malloc(sizeof(AppData));
    app_context->data->buffer = furi_string_alloc();

    // Set the initial state of the laser pointer.
    attempt_set_5v_state(false);

    // Queue for events (tick or input)
    app_context->queue = furi_message_queue_alloc(8, sizeof(AppEvent));

    // Set ViewPort callbacks
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, app_context);
    view_port_input_callback_set(
        view_port, (ViewPortInputCallback)input_callback, app_context->queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // Main loop
    AppEvent event;
    bool processing = true;
    do {
        if(furi_message_queue_get(app_context->queue, &event, FuriWaitForever) == FuriStatusOk) {
            FURI_LOG_T(TAG, "Got event type: %d", event.type);
            if(event.type == AppEventTypeKey) {
                FURI_LOG_T(
                    TAG, "Got key event: %d and type: %d", event.input.key, event.input.type);
                // InputTypeLong is only fired after a delay, so this compensates for that delay.
                // since we will always keep the laser on when the button is pressed.
                if(event.input.key == InputKeyOk && event.input.type == InputTypePress) {
                    if(furi_mutex_acquire(app_context->mutex, FuriWaitForever) == FuriStatusOk) {
                        app_context->data->pressed = true;
                        furi_mutex_release(app_context->mutex);
                    }
                }

                if(event.input.key == InputKeyBack) {
                    FURI_LOG_I(TAG, "Back pressed. Exiting program.");
                    processing = false;
                } else if(event.input.type == InputTypeShort && event.input.key == InputKeyOk) {
                    FURI_LOG_I(TAG, "Short-OK pressed.");
                    if(furi_mutex_acquire(app_context->mutex, FuriWaitForever) == FuriStatusOk) {
                        app_context->data->keep_on = !app_context->data->keep_on; // toggle
                        app_context->data->pressed = false;
                        app_context->data->short_expect_release = true;
                        furi_mutex_release(app_context->mutex);
                    }
                } else if(
                    event.input.key == InputKeyOk &&
                    (event.input.type == InputTypeRelease || event.input.type == InputTypeLong)) {
                    FURI_LOG_I(TAG, "OK pressed/released");
                    if(furi_mutex_acquire(app_context->mutex, FuriWaitForever) == FuriStatusOk) {
                        if(app_context->data->short_expect_release) { // ignore short press release
                            app_context->data->short_expect_release = false;
                        } else {
                            app_context->data->pressed =
                                event.input.type == InputTypeRelease ? false : true;
                        }
                        furi_mutex_release(app_context->mutex);
                    }
                }
            }
            update_pointer_state(app_context);
            // Send signal to update the screen (callback will get invoked at some point later.)
            view_port_update(view_port);
        } else {
            // We had an issue getting message from the queue, so exit application.
            processing = false;
        }
    } while(processing);

    // Free resources
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app_context->queue);
    furi_mutex_free(app_context->mutex);
    furi_string_free(app_context->data->buffer);
    free(app_context->data);
    free(app_context);

    return 0;
}
