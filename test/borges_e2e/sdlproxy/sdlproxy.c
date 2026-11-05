// Built as SDL2.dll and placed next to a copy of program.exe, in front of the real SDL2
// renamed SDL2_real.dll. The firmware and the simulator stay byte for byte what everyone
// else builds. What it adds is the one thing an env-var schedule cannot give: a test that
// reacts to what the screen and the log say.
//
// A TCP connection to the harness (port in E2E_SIM_CONTROL_PORT), opened per process:
//   proxy   -> harness   HELLO <pid>
//                        FRAME <n> <fnv1a of the presented pixels>
//                        SHOT <id> <width> <height>     after writing raw BGRA to the path
//   harness -> proxy     KEY <scancode> <holdMs>
//                        QUIT
//                        SHOT <id> <path>
//
// Every SDL entry the firmware calls is forwarded (sdlproxy.def); these three are wrapped.
// A key press arrives as a real SDL_KEYDOWN/KEYUP, the same path a keyboard takes.

#define WIN32_LEAN_AND_MEAN
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

typedef int(SDLCALL* PollFn)(SDL_Event*);
typedef const Uint8*(SDLCALL* KeyStateFn)(int*);
typedef void(SDLCALL* PresentFn)(SDL_Renderer*);
typedef int(SDLCALL* OutputSizeFn)(SDL_Renderer*, int*, int*);
typedef int(SDLCALL* ReadPixelsFn)(SDL_Renderer*, const SDL_Rect*, Uint32, void*, int);
typedef Uint32(SDLCALL* TicksFn)(void);

static PollFn realPoll;
static KeyStateFn realKeyState;
static PresentFn realPresent;
static OutputSizeFn realOutputSize;
static ReadPixelsFn realReadPixels;
static TicksFn realTicks;

static SOCKET control = INVALID_SOCKET;
static int triedConnect = 0;
static char lineBuf[2048];
static int lineLen = 0;

#define MAX_PENDING 64
typedef struct {
  Uint32 atTick;
  int scancode;
  int down;
  int quit;
  int used;
} Pending;
static Pending pending[MAX_PENDING];
static Uint8 forcedDown[SDL_NUM_SCANCODES];
static Uint8 mergedState[SDL_NUM_SCANCODES];

static Uint32* lastFrame;
static int lastWidth, lastHeight;
static unsigned long frameCounter;

static void loadReal(void) {
  if (realPoll) return;
  HMODULE sdl = LoadLibraryA("SDL2_real.dll");
  if (!sdl) {
    fputs("sdlproxy: cannot load SDL2_real.dll\n", stderr);
    ExitProcess(3);
  }
  realKeyState = (KeyStateFn)GetProcAddress(sdl, "SDL_GetKeyboardState");
  realPresent = (PresentFn)GetProcAddress(sdl, "SDL_RenderPresent");
  realOutputSize = (OutputSizeFn)GetProcAddress(sdl, "SDL_GetRendererOutputSize");
  realReadPixels = (ReadPixelsFn)GetProcAddress(sdl, "SDL_RenderReadPixels");
  realTicks = (TicksFn)GetProcAddress(sdl, "SDL_GetTicks");
  realPoll = (PollFn)GetProcAddress(sdl, "SDL_PollEvent");
}

static void sendLine(const char* text) {
  if (control == INVALID_SOCKET) return;
  int left = (int)strlen(text);
  while (left > 0) {
    const int sent = send(control, text, left, 0);
    if (sent == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) {
        Sleep(1);
        continue;
      }
      closesocket(control);
      control = INVALID_SOCKET;
      return;
    }
    text += sent;
    left -= sent;
  }
}

static void connectOnce(void) {
  if (triedConnect) return;
  triedConnect = 1;
  const char* port = getenv("E2E_SIM_CONTROL_PORT");
  if (!port || !port[0]) return;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)atoi(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    closesocket(s);
    return;
  }
  BOOL noDelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
  u_long nonBlocking = 1;
  ioctlsocket(s, FIONBIO, &nonBlocking);
  control = s;
  char hello[64];
  snprintf(hello, sizeof(hello), "HELLO %lu\n", (unsigned long)GetCurrentProcessId());
  sendLine(hello);
}

static void addPending(const Uint32 atTick, const int scancode, const int down, const int quit) {
  for (int i = 0; i < MAX_PENDING; ++i) {
    if (!pending[i].used) {
      pending[i] = (Pending){atTick, scancode, down, quit, 1};
      return;
    }
  }
}

static void takeShot(const char* id, const char* path) {
  char reply[128];
  int ok = 0;
  if (lastFrame) {
    FILE* f = fopen(path, "wb");
    if (f) {
      ok = fwrite(lastFrame, 4, (size_t)lastWidth * lastHeight, f) == (size_t)lastWidth * lastHeight;
      fclose(f);
    }
  }
  snprintf(reply, sizeof(reply), "SHOT %s %d %d\n", id, ok ? lastWidth : 0, ok ? lastHeight : 0);
  sendLine(reply);
}

static void handleLine(char* line) {
  char* save = NULL;
  const char* verb = strtok_r(line, " ", &save);
  if (!verb) return;
  if (strcmp(verb, "KEY") == 0) {
    const char* code = strtok_r(NULL, " ", &save);
    const char* hold = strtok_r(NULL, " ", &save);
    if (!code) return;
    const Uint32 now = realTicks();
    addPending(now, atoi(code), 1, 0);
    addPending(now + (Uint32)(hold ? atoi(hold) : 80), atoi(code), 0, 0);
  } else if (strcmp(verb, "QUIT") == 0) {
    addPending(realTicks(), 0, 0, 1);
  } else if (strcmp(verb, "SHOT") == 0) {
    const char* id = strtok_r(NULL, " ", &save);
    const char* path = strtok_r(NULL, "", &save);
    if (id && path) takeShot(id, path);
  }
}

static void readControl(void) {
  if (control == INVALID_SOCKET) return;
  char chunk[512];
  for (;;) {
    const int got = recv(control, chunk, sizeof(chunk), 0);
    if (got == SOCKET_ERROR) {
      if (WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(control);
        control = INVALID_SOCKET;
      }
      return;
    }
    if (got == 0) {
      closesocket(control);
      control = INVALID_SOCKET;
      return;
    }
    for (int i = 0; i < got; ++i) {
      if (chunk[i] == '\n') {
        lineBuf[lineLen] = '\0';
        handleLine(lineBuf);
        lineLen = 0;
      } else if (lineLen + 1 < (int)sizeof(lineBuf)) {
        lineBuf[lineLen++] = chunk[i];
      }
    }
  }
}

__declspec(dllexport) int SDLCALL SDL_PollEvent(SDL_Event* event) {
  loadReal();
  connectOnce();
  readControl();
  const Uint32 now = realTicks();
  int oldest = -1;
  for (int i = 0; i < MAX_PENDING; ++i) {
    if (!pending[i].used || (Sint32)(now - pending[i].atTick) < 0) continue;
    if (oldest < 0 || (Sint32)(pending[oldest].atTick - pending[i].atTick) > 0) oldest = i;
  }
  if (oldest >= 0) {
    const Pending p = pending[oldest];
    pending[oldest].used = 0;
    if (event) {
      memset(event, 0, sizeof(*event));
      if (p.quit) {
        event->type = SDL_QUIT;
      } else {
        event->type = p.down ? SDL_KEYDOWN : SDL_KEYUP;
        event->key.state = p.down ? SDL_PRESSED : SDL_RELEASED;
        event->key.keysym.scancode = (SDL_Scancode)p.scancode;
        if (p.scancode >= 0 && p.scancode < SDL_NUM_SCANCODES) forcedDown[p.scancode] = p.down ? 1 : 0;
      }
    }
    return 1;
  }
  return realPoll(event);
}

__declspec(dllexport) const Uint8* SDLCALL SDL_GetKeyboardState(int* numkeys) {
  loadReal();
  const Uint8* real = realKeyState(numkeys);
  for (int i = 0; i < SDL_NUM_SCANCODES; ++i) mergedState[i] = (real ? real[i] : 0) | forcedDown[i];
  return mergedState;
}

__declspec(dllexport) void SDLCALL SDL_RenderPresent(SDL_Renderer* renderer) {
  loadReal();
  connectOnce();
  int width = 0;
  int height = 0;
  if (realOutputSize(renderer, &width, &height) == 0 && width > 0 && height > 0) {
    if (!lastFrame || width != lastWidth || height != lastHeight) {
      free(lastFrame);
      lastFrame = (Uint32*)malloc((size_t)width * height * 4);
      lastWidth = width;
      lastHeight = height;
    }
    if (lastFrame && realReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, lastFrame, width * 4) == 0) {
      Uint32 hash = 2166136261u;
      const size_t count = (size_t)width * height;
      for (size_t i = 0; i < count; ++i) hash = (hash ^ lastFrame[i]) * 16777619u;
      char line[64];
      snprintf(line, sizeof(line), "FRAME %lu %08x\n", ++frameCounter, hash);
      sendLine(line);
    }
  }
  realPresent(renderer);
}
