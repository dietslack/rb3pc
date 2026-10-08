// padprobe: list SDL gamepads (type, IDs, mapping) and optionally print every
// button/axis change for N seconds. Usage: padprobe [seconds]
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 0;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i) {
    SDL_Joystick* joy = SDL_OpenJoystick(ids[i]);
    printf("joystick %u: \"%s\" vid=%04X pid=%04X joytype=%d axes=%d buttons=%d hats=%d\n", ids[i],
           SDL_GetJoystickName(joy), SDL_GetJoystickVendor(joy), SDL_GetJoystickProduct(joy),
           SDL_GetJoystickType(joy), SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickButtons(joy),
           SDL_GetNumJoystickHats(joy));
    if (SDL_IsGamepad(ids[i])) {
      SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
      char* mapping = SDL_GetGamepadMapping(pad);
      printf("  gamepad type=%d mapping: %s\n", SDL_GetGamepadType(pad), mapping ? mapping : "(none)");
      SDL_free(mapping);
    }
  }
  SDL_free(ids);
  fflush(stdout);

  Uint64 end = SDL_GetTicks() + (Uint64)seconds * 1000;
  SDL_Event e;
  while (SDL_GetTicks() < end) {
    while (SDL_WaitEventTimeout(&e, 50)) {
      switch (e.type) {
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
          printf("[%6llu] joy %u raw button %d %s\n", (unsigned long long)SDL_GetTicks(),
                 e.jbutton.which, e.jbutton.button, e.jbutton.down ? "down" : "up");
          break;
        case SDL_EVENT_JOYSTICK_HAT_MOTION:
          printf("[%6llu] joy %u raw hat %d = %d\n", (unsigned long long)SDL_GetTicks(),
                 e.jhat.which, e.jhat.hat, e.jhat.value);
          break;
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
          printf("[%6llu] joy %u raw axis %d = %d\n", (unsigned long long)SDL_GetTicks(),
                 e.jaxis.which, e.jaxis.axis, e.jaxis.value);
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
          printf("[%6llu] pad %u button %s %s\n", (unsigned long long)SDL_GetTicks(),
                 e.gbutton.which, SDL_GetGamepadStringForButton(e.gbutton.button),
                 e.gbutton.down ? "down" : "up");
          break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
          printf("[%6llu] pad %u axis %s = %d\n", (unsigned long long)SDL_GetTicks(), e.gaxis.which,
                 SDL_GetGamepadStringForAxis(e.gaxis.axis), e.gaxis.value);
          break;
      }
      fflush(stdout);
    }
  }
  SDL_Quit();
  return 0;
}
