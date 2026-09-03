#pragma once
#include <vector>

typedef enum {
  STATE_IDLE,
  STATE_RECORDING,
  STATE_SAVED,
  STATE_TAG_SELECT,
  STATE_MENU,
  STATE_TAG_BROWSER,
  STATE_NOTE_LIST,
  STATE_NOTE_DETAIL,
  STATE_DELETE_CONFIRM,
  STATE_SETTINGS,
  STATE_DEVICE_INFO,
  STATE_TRANSFER,
  STATE_ERROR
} AppState;

enum ButtonEvent { EV_NONE, EV_SINGLE, EV_LONG, EV_DOUBLE };

// `uploaded` means the note reached the notes server. It says nothing
// about whether the processed text has come back - that is answered by
// noteHasLocalText(), which tests for the .txt file on the card.
struct NoteEntry { int num; char tag[32]; bool uploaded; };

// Content array sizes — used across notes, ui, and main loop.
#define DEFAULT_TAG_COUNT 5
#define MENU_COUNT        4
#define SETTINGS_COUNT    3

extern const char* DEFAULT_TAGS[];
extern const char* MENU_ITEMS[];
extern const char* SETTINGS_ITEMS[];
