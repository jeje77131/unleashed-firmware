#include "fix_tests.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <toolbox/version.h>

// One past the largest length a uint8_t holds: kept in one, it reads as 0
#define KEYBOARD_LENGTH      (256U)
#define KEYBOARD_BUFFER_SIZE (512U)

#define LOG_PATH APP_DATA_PATH("results.txt")

typedef enum {
    FixTestsViewMenu,
    FixTestsViewDialog,
    FixTestsViewKeyboard,
    FixTestsViewText,
} FixTestsView;

typedef enum {
    FixTestsItemKeyboard,
    FixTestsItemGps,
    FixTestsItemNetwork,
    FixTestsItemRealloc,
    FixTestsItemCalloc,
    FixTestsItemAbout,
} FixTestsItem;

typedef enum {
    FixTestsEventKeyboardDone,
    FixTestsEventDialogRun,
    FixTestsEventDialogBack,
} FixTestsEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    DialogEx* dialog;
    TextInput* text_input;
    TextBox* text_box;
    // The text box keeps a pointer to what it shows and lays it out as it draws, on the GUI
    // thread: a text is only rewritten once the box has been handed the other one
    FuriString* text[2];
    size_t text_shown;
    char* keyboard;
    const char* firmware;
    FixTestsItem pending; // test the dialog's Run button starts
} FixTestsApp;

static void fix_tests_log(FixTestsApp* app, const char* line) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FuriString* entry = furi_string_alloc_printf("[%s] %s\n", app->firmware, line);
        storage_file_write(file, furi_string_get_cstr(entry), furi_string_size(entry));
        furi_string_free(entry);
    }
    // Closing writes it out, so a line survives a crash that comes right after it
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static FuriString* fix_tests_text_begin(FixTestsApp* app) {
    FuriString* text = app->text[app->text_shown ^ 1];
    furi_string_reset(text);
    return text;
}

static void fix_tests_text_show(FixTestsApp* app) {
    app->text_shown ^= 1;
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->text[app->text_shown]));
    view_dispatcher_switch_to_view(app->view_dispatcher, FixTestsViewText);
}

static void fix_tests_keyboard_callback(void* context) {
    FixTestsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, FixTestsEventKeyboardDone);
}

static void fix_tests_keyboard_start(FixTestsApp* app) {
    for(size_t i = 0; i < KEYBOARD_LENGTH; i++) {
        app->keyboard[i] = (char)('0' + i % 10);
    }
    app->keyboard[KEYBOARD_LENGTH] = '\0';

    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Type 1 letter, Save");
    text_input_set_result_callback(
        app->text_input,
        fix_tests_keyboard_callback,
        app,
        app->keyboard,
        KEYBOARD_BUFFER_SIZE,
        false);
    view_dispatcher_switch_to_view(app->view_dispatcher, FixTestsViewKeyboard);
}

static void fix_tests_keyboard_report(FixTestsApp* app) {
    const size_t length = strlen(app->keyboard);
    size_t at = 0;
    while(at < KEYBOARD_LENGTH && app->keyboard[at] == (char)('0' + at % 10)) {
        at++;
    }

    FuriString* text = fix_tests_text_begin(app);
    furi_string_cat_str(text, "1 Keyboard, 256 chars\n\n");
    if(length == KEYBOARD_LENGTH + 1 && at == KEYBOARD_LENGTH) {
        furi_string_cat_str(
            text,
            "Letter added at 256, the end.\n\n"
            "OK: the keyboard kept the whole text and the cursor at its end.");
        fix_tests_log(app, "1 keyboard: OK, letter added at 256 of 256");
    } else if(length == KEYBOARD_LENGTH + 1 && at == 0) {
        furi_string_cat_str(
            text,
            "Letter added at 0, expected 256.\n\n"
            "BUG: the keyboard keeps the length in 8 bits. It took the 256 chars for none "
            "and put the cursor at the start.");
        fix_tests_log(app, "1 keyboard: BUG, letter added at 0 instead of 256");
    } else if(length == KEYBOARD_LENGTH && at == KEYBOARD_LENGTH) {
        furi_string_cat_str(text, "Nothing was typed. Run it again: one letter, then Save.");
    } else {
        furi_string_cat_printf(
            text,
            "Text changed at %lu, length %lu.\n\n"
            "Run it again and type one letter only, without moving the cursor.",
            (unsigned long)at,
            (unsigned long)length);
    }
    fix_tests_text_show(app);
}

static void fix_tests_race(FixTestsApp* app, FixTestsRaceTarget target) {
    const bool gps = target == FixTestsRaceGps;
    const char* title = gps ? "3a GPS callback" : "3b Network callback";

    FuriString* text = fix_tests_text_begin(app);
    furi_string_printf(text, "%s\n\nRunning, about 2 s...", title);
    fix_tests_text_show(app);

    FixTestsRaceResult result;
    fix_tests_race_run(target, &result);

    text = fix_tests_text_begin(app);
    furi_string_printf(
        text,
        "%s\n\nCalls: %lu\nLate: %lu\nRounds: %lu\n\n",
        title,
        (unsigned long)result.calls,
        (unsigned long)result.late,
        (unsigned long)result.rounds);

    const char* verdict;
    if(result.calls == 0) {
        verdict = "INCONCLUSIVE";
        furi_string_cat_str(text, "INCONCLUSIVE: the service delivered nothing.");
    } else if(result.late) {
        verdict = "BUG";
        furi_string_cat_str(
            text,
            "BUG: the callback still ran after unsubscribing returned. An app that frees "
            "its context there has it used after free.");
    } else {
        verdict = "OK";
        furi_string_cat_str(text, "OK: unsubscribing waits for a running callback.");
    }
    furi_string_cat_str(
        text, "\n\nIf the phone app was connected, reconnect it to restore its GPS and network.");

    FuriString* line = furi_string_alloc_printf(
        "%s %s: %s, late %lu of %lu calls, %lu rounds",
        gps ? "3a" : "3b",
        gps ? "gps" : "network",
        verdict,
        (unsigned long)result.late,
        (unsigned long)result.calls,
        (unsigned long)result.rounds);
    fix_tests_log(app, furi_string_get_cstr(line));
    furi_string_free(line);

    fix_tests_text_show(app);
}

static void fix_tests_realloc(FixTestsApp* app) {
    FixTestsReallocResult result;
    fix_tests_realloc_run(&result);

    FuriString* text = fix_tests_text_begin(app);
    furi_string_printf(
        text,
        "5a realloc\n\nA 16-byte block grown to 48.\nNon-zero bytes past the old 16: %lu\n"
        "Neighbour block data: %s\n\n",
        (unsigned long)result.outside,
        result.neighbour ? "yes" : "no");

    const char* verdict;
    if(!result.kept) {
        verdict = "ERROR";
        furi_string_cat_str(text, "ERROR: the old block's own bytes were lost.");
    } else if(result.outside) {
        verdict = "BUG";
        furi_string_cat_str(
            text, "BUG: realloc read past the end of the old block and copied what lay there.");
    } else {
        verdict = "OK";
        furi_string_cat_str(text, "OK: realloc copied the old block only.");
    }

    FuriString* line = furi_string_alloc_printf(
        "5a realloc: %s, %lu non-zero bytes past the old block, neighbour data %s",
        verdict,
        (unsigned long)result.outside,
        result.neighbour ? "copied" : "not seen");
    fix_tests_log(app, furi_string_get_cstr(line));
    furi_string_free(line);

    fix_tests_text_show(app);
}

static void fix_tests_calloc(FixTestsApp* app) {
    // Logged first: patched firmware stops inside calloc(), and this line is then the last one
    fix_tests_log(app, "5b calloc: started; as the last line, the firmware stopped in calloc");

    const bool returned = fix_tests_calloc_run();

    FuriString* text = fix_tests_text_begin(app);
    furi_string_cat_str(text, "5b calloc overflow\n\n");
    if(returned) {
        furi_string_cat_str(
            text,
            "BUG: 1073741825 x 4 wrapped to 4 bytes, and calloc returned a 4-byte block for "
            "a 4 GiB request.");
        fix_tests_log(app, "5b calloc: BUG, a 4 GiB request got a 4-byte block");
    } else {
        // malloc() never returns NULL on this firmware: only here for completeness
        furi_string_cat_str(text, "calloc returned NULL.");
        fix_tests_log(app, "5b calloc: returned NULL");
    }
    fix_tests_text_show(app);
}

static void fix_tests_about(FixTestsApp* app) {
    FuriString* text = fix_tests_text_begin(app);
    furi_string_printf(
        text,
        "Fix Tests 095\nFirmware: %s\n\n"
        "Defects 1, 3 and 5 of the unlshd-095 review do not show in normal use. Each test "
        "reports BUG on unpatched firmware and OK on patched.\n\n"
        "1: the keyboard keeps the text length in 8 bits.\n"
        "3a, 3b: GPS and network callbacks still run after unsubscribing.\n"
        "5a: realloc reads past the old block.\n"
        "5b: calloc size overflow. Patched firmware stops on 'out of memory' and reboots.\n\n"
        "Results are added to apps_data/unlshd_fix_tests/results.txt",
        app->firmware);
    fix_tests_text_show(app);
}

static void fix_tests_dialog_callback(DialogExResult result, void* context) {
    FixTestsApp* app = context;
    if(result == DialogExResultRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, FixTestsEventDialogRun);
    } else if(result == DialogExResultLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, FixTestsEventDialogBack);
    }
}

static void fix_tests_dialog(
    FixTestsApp* app,
    FixTestsItem item,
    const char* header,
    const char* text,
    const char* run) {
    app->pending = item;
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(app->dialog, text, 64, 13, AlignCenter, AlignTop);
    dialog_ex_set_left_button_text(app->dialog, "Back");
    dialog_ex_set_right_button_text(app->dialog, run);
    dialog_ex_set_result_callback(app->dialog, fix_tests_dialog_callback);
    dialog_ex_set_context(app->dialog, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, FixTestsViewDialog);
}

static void fix_tests_menu_callback(void* context, uint32_t index) {
    FixTestsApp* app = context;

    switch(index) {
    case FixTestsItemKeyboard:
        fix_tests_dialog(
            app,
            FixTestsItemKeyboard,
            "1 Keyboard, 256 chars",
            "The field holds 256 digits.\nType ONE letter, then Save.\nBug: the field looks empty.",
            "Start");
        break;
    case FixTestsItemGps:
        fix_tests_race(app, FixTestsRaceGps);
        break;
    case FixTestsItemNetwork:
        fix_tests_race(app, FixTestsRaceNetwork);
        break;
    case FixTestsItemRealloc:
        fix_tests_realloc(app);
        break;
    case FixTestsItemCalloc:
        fix_tests_dialog(
            app,
            FixTestsItemCalloc,
            "5b calloc overflow",
            "Asks 1073741825 x 4 bytes.\nPatched firmware stops on\n'out of memory', reboots.",
            "Run");
        break;
    case FixTestsItemAbout:
        fix_tests_about(app);
        break;
    default:
        break;
    }
}

static bool fix_tests_custom_event_callback(void* context, uint32_t event) {
    FixTestsApp* app = context;

    switch(event) {
    case FixTestsEventKeyboardDone:
        fix_tests_keyboard_report(app);
        return true;
    case FixTestsEventDialogRun:
        if(app->pending == FixTestsItemKeyboard) {
            fix_tests_keyboard_start(app);
        } else if(app->pending == FixTestsItemCalloc) {
            fix_tests_calloc(app);
        }
        return true;
    case FixTestsEventDialogBack:
        view_dispatcher_switch_to_view(app->view_dispatcher, FixTestsViewMenu);
        return true;
    default:
        return false;
    }
}

static uint32_t fix_tests_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t fix_tests_back_to_menu(void* context) {
    UNUSED(context);
    return FixTestsViewMenu;
}

static FixTestsApp* fix_tests_alloc(void) {
    FixTestsApp* app = malloc(sizeof(FixTestsApp));

    app->keyboard = malloc(KEYBOARD_BUFFER_SIZE);
    app->text[0] = furi_string_alloc();
    app->text[1] = furi_string_alloc();

    const Version* version = furi_hal_version_get_firmware_version();
    app->firmware = version ? version_get_version(version) : NULL;
    if(!app->firmware) app->firmware = "unknown";

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, fix_tests_custom_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, app->firmware);
    submenu_add_item(
        app->submenu, "1 Keyboard 256 chars", FixTestsItemKeyboard, fix_tests_menu_callback, app);
    submenu_add_item(
        app->submenu, "3a GPS callback", FixTestsItemGps, fix_tests_menu_callback, app);
    submenu_add_item(
        app->submenu, "3b Network callback", FixTestsItemNetwork, fix_tests_menu_callback, app);
    submenu_add_item(
        app->submenu, "5a realloc", FixTestsItemRealloc, fix_tests_menu_callback, app);
    submenu_add_item(
        app->submenu, "5b calloc (reboots)", FixTestsItemCalloc, fix_tests_menu_callback, app);
    submenu_add_item(app->submenu, "About", FixTestsItemAbout, fix_tests_menu_callback, app);
    View* view = submenu_get_view(app->submenu);
    view_set_previous_callback(view, fix_tests_exit);
    view_dispatcher_add_view(app->view_dispatcher, FixTestsViewMenu, view);

    app->dialog = dialog_ex_alloc();
    view = dialog_ex_get_view(app->dialog);
    view_set_previous_callback(view, fix_tests_back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, FixTestsViewDialog, view);

    app->text_input = text_input_alloc();
    view = text_input_get_view(app->text_input);
    view_set_previous_callback(view, fix_tests_back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, FixTestsViewKeyboard, view);

    app->text_box = text_box_alloc();
    view = text_box_get_view(app->text_box);
    view_set_previous_callback(view, fix_tests_back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, FixTestsViewText, view);

    view_dispatcher_switch_to_view(app->view_dispatcher, FixTestsViewMenu);
    return app;
}

static void fix_tests_free(FixTestsApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, FixTestsViewText);
    view_dispatcher_remove_view(app->view_dispatcher, FixTestsViewKeyboard);
    view_dispatcher_remove_view(app->view_dispatcher, FixTestsViewDialog);
    view_dispatcher_remove_view(app->view_dispatcher, FixTestsViewMenu);
    text_box_free(app->text_box);
    text_input_free(app->text_input);
    dialog_ex_free(app->dialog);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    furi_string_free(app->text[1]);
    furi_string_free(app->text[0]);
    free(app->keyboard);
    free(app);
}

int32_t fix_tests_app(void* p) {
    UNUSED(p);

    FixTestsApp* app = fix_tests_alloc();
    view_dispatcher_run(app->view_dispatcher);
    fix_tests_free(app);

    return 0;
}
