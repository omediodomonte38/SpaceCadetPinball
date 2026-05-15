#include "pch.h"
#include "winmain.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "high_score.h"
#include "control.h"
#include "EmbeddedData.h"
#include "fullscrn.h"
#include "midi.h"
#include "options.h"
#include "pb.h"
#include "render.h"
#include "Sound.h"
#include "translations.h"
#include "font_selection.h"

constexpr const char* winmain::Version;

SDL_Window* winmain::MainWindow = nullptr;
SDL_Renderer* winmain::Renderer = nullptr;
ImGuiIO* winmain::ImIO = nullptr;

// Session-helper captures. WinMain sets these once, then StartSession and EndSession
// read them so the native restart loop and the in-callback restart used on
// Emscripten share one code path.
static const char* s_lpCmdLine = "";
static char*       s_basePath = nullptr;
static char*       s_prefPath = nullptr;
static bool        s_mixOpened = false;
static bool        s_resetAllOptions = false;
static std::string s_iniPath;

int winmain::return_value = 0;
bool winmain::bQuit = false;
bool winmain::activated = false;
bool winmain::DispFrameRate = false;
bool winmain::DispGRhistory = false;
bool winmain::single_step = false;
bool winmain::has_focus = true;
int winmain::last_mouse_x;
int winmain::last_mouse_y;
int winmain::mouse_down;
bool winmain::no_time_loss = false;

bool winmain::restart = false;

std::vector<float> winmain::gfrDisplay{};
unsigned winmain::gfrOffset = 0;
float winmain::gfrWindow = 5.0f;
bool winmain::ShowAboutDialog = false;
bool winmain::ShowImGuiDemo = false;
bool winmain::ShowSpriteViewer = false;
bool winmain::ShowExitPopup = false;
bool winmain::LaunchBallEnabled = true;
bool winmain::HighScoresEnabled = true;
bool winmain::DemoActive = false;
int winmain::MainMenuHeight = 0;
std::string winmain::FpsDetails, winmain::PrevSdlError;
unsigned winmain::PrevSdlErrorCount = 0;
double winmain::UpdateToFrameRatio;
winmain::DurationMs winmain::TargetFrameTime;
optionsStruct& winmain::Options = options::Options;
winmain::DurationMs winmain::SpinThreshold = DurationMs(0.005);
WelfordState winmain::SleepState{};
int winmain::CursorIdleCounter = 0;

int winmain::WinMain(LPCSTR lpCmdLine)
{
	std::set_new_handler(memalloc_failure);

	printf("Game version: %s\n", Version);
	printf("Command line: %s\n", lpCmdLine);
	printf("Compiled with: SDL %d.%d.%d;", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
	printf(" SDL_mixer %d.%d.%d;", SDL_MIXER_MAJOR_VERSION, SDL_MIXER_MINOR_VERSION, SDL_MIXER_PATCHLEVEL);
	printf(" ImGui %s %s\n", IMGUI_VERSION, ImGuiRender);

	// SDL init
	SDL_SetMainReady();
	// Disable SDL's touch<->mouse synthesis. Our shell already dispatches
	// touches as either game-action keys or as synthetic mouse clicks
	// (for menu and modal regions), so without this every touch would fire both.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO |
		SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
	{
		pb::ShowMessageBox(SDL_MESSAGEBOX_ERROR, "Could not initialize SDL2", SDL_GetError());
		return 1;
	}

	pb::quickFlag = strstr(lpCmdLine, "-quick") != nullptr;

	// SDL window
	SDL_Window* window = SDL_CreateWindow
	(
		pb::get_rc_string(Msg::STRING139),
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		800, 556,
		SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE
	);
	MainWindow = window;
	if (!window)
	{
		pb::ShowMessageBox(SDL_MESSAGEBOX_ERROR, "Could not create window", SDL_GetError());
		return 1;
	}

	// If HW fails, fallback to SW SDL renderer.
	SDL_Renderer* renderer = nullptr;
	// Emscripten's opengles2 backend in SDL 2.32 fails to compile its internal
	// shader on WebGL ("GL_OES_EGL_image_external extension is not supported"),
	// so HW renders black. SW is reliable and fast enough for this game.
#ifdef __EMSCRIPTEN__
	auto swOffset = 1;
#else
	auto swOffset = strstr(lpCmdLine, "-sw") != nullptr ? 1 : 0;
#endif
	for (int i = swOffset; i < 2 && !renderer; i++)
	{
		Renderer = renderer = SDL_CreateRenderer
		(
			window,
			-1,
			i == 0 ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE
		);
	}
	if (!renderer)
	{
		pb::ShowMessageBox(SDL_MESSAGEBOX_ERROR, "Could not create renderer", SDL_GetError());
		return 1;
	}
	SDL_RendererInfo rendererInfo{};
	if (!SDL_GetRendererInfo(renderer, &rendererInfo))
		printf("Using SDL renderer: %s\n", rendererInfo.name);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	s_prefPath = SDL_GetPrefPath("", "SpaceCadetPinball");
	s_basePath = SDL_GetBasePath();

	// SDL mixer init
	bool mixOpened = false, noAudio = strstr(lpCmdLine, "-noaudio") != nullptr;
	if (!noAudio)
	{
#ifndef MUSIC_TSF
		// With MUSIC_TSF, MIDI is synthesized by TinySoundFont, so SDL_mixer's
		// MIDI backend is not needed
		if ((Mix_Init(MIX_INIT_MID_Proxy) & MIX_INIT_MID_Proxy) == 0)
		{
			printf("Could not initialize SDL MIDI, music might not work.\nSDL Error: %s\n", SDL_GetError());
			SDL_ClearError();
		}
#endif
		if (Mix_OpenAudio(MIX_DEFAULT_FREQUENCY, MIX_DEFAULT_FORMAT, 2, 1024) != 0)
		{
			printf("Could not open audio device, continuing without audio.\nSDL Error: %s\n", SDL_GetError());
			SDL_ClearError();
		}
		else
			mixOpened = true;
	}
	s_mixOpened = mixOpened;
	s_lpCmdLine = lpCmdLine;

	{
		// Load SDL Game Controller definitions from DB
		unsigned decompressedSize{};
		const auto controllerDb = ImFontAtlas::DecompressCompressedStbData(
			EmbeddedData::SDL_GameControllerDB_compressed_data,
			EmbeddedData::SDL_GameControllerDB_compressed_size,
			decompressedSize);
		auto rw = SDL_RWFromMem(controllerDb, decompressedSize);
		const auto added = SDL_GameControllerAddMappingsFromRW(rw, 1);
		IM_FREE(controllerDb);
		if (added < 0)
		{
			printf("Could not load game controller DB.\nSDL Error: %s\n", SDL_GetError());
			SDL_ClearError();
		}
	}

	s_resetAllOptions = strstr(lpCmdLine, "-reset") != nullptr;

	// Native: do { Start -> MainLoop -> End } while (restart). On Emscripten
	// MainLoop() never returns (the rAF loop unwinds C++ via simulate_infinite
	// _loop=1), so the do-while degenerates to a single iteration and the
	// in-callback restart path inside MainLoopIteration takes over.
	do
	{
		restart = false;
		if (auto err = StartSession())
			return err;
		MainLoop();
		EndSession();
	}
	while (restart);

	if (!noAudio)
	{
		if (mixOpened)
			Mix_CloseAudio();
		Mix_Quit();
	}

	SDL_free(s_basePath);
	SDL_free(s_prefPath);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return return_value;
}

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE void web_flush_persistence()
{
	winmain::WebFlushPersistence();
}

// Web touch path. JS dispatches a GameBindings action, we look up the user's
// first keyboard binding for it and push a synthetic SDL_KEYDOWN/UP so the
// normal scancode → pb::InputDown → HandleGameBinding pipeline runs.
static SDL_Scancode FirstKeyboardScancode(GameBindings action)
{
	if (action < GameBindings::Min || action >= GameBindings::Max)
		return SDL_SCANCODE_UNKNOWN;
	auto& opt = options::Options.Key[static_cast<int>(action)];
	for (int i = 0; i < 3; ++i)
	{
		if (opt.Inputs[i].Type == InputTypes::Keyboard && opt.Inputs[i].Value != 0)
			return static_cast<SDL_Scancode>(opt.Inputs[i].Value);
	}
	return SDL_SCANCODE_UNKNOWN;
}

static void PushSynthKeyEvent(uint32_t type, SDL_Scancode sc)
{
	if (sc == SDL_SCANCODE_UNKNOWN) return;
	SDL_Event ev{};
	ev.type = type;
	ev.key.timestamp = SDL_GetTicks();
	ev.key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat = 0;
	ev.key.keysym.scancode = sc;
	ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
	SDL_PushEvent(&ev);
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_touch_down(int actionId)
{
	// First touch dismisses the one-shot tutorial overlay
	if (options::Options.ShowTouchHints)
		options::Options.ShowTouchHints = false;
	PushSynthKeyEvent(SDL_KEYDOWN, FirstKeyboardScancode(static_cast<GameBindings>(actionId)));
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_touch_up(int actionId)
{
	PushSynthKeyEvent(SDL_KEYUP,   FirstKeyboardScancode(static_cast<GameBindings>(actionId)));
}

// the JS touch handler asks the routing helpers to decide
// whether a given touch should fire a game action (flipper/plunger) or
// be treated as a UI click (menu bar, modal buttons).
extern "C" EMSCRIPTEN_KEEPALIVE int web_menu_bar_height()
{
	return options::Options.ShowMenu ? winmain::MainMenuHeight : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int web_modal_open()
{
	return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ? 1 : 0;
}

// Motion must precede the button event so ImGui's hover state lands on the
// right widget before the click.
static void PushSynthMouseAt(uint32_t type, int x, int y)
{
	SDL_Event motion{};
	motion.type = SDL_MOUSEMOTION;
	motion.motion.timestamp = SDL_GetTicks();
	motion.motion.x = x;
	motion.motion.y = y;
	SDL_PushEvent(&motion);

	SDL_Event btn{};
	btn.type = type;
	btn.button.timestamp = SDL_GetTicks();
	btn.button.button = SDL_BUTTON_LEFT;
	btn.button.state = (type == SDL_MOUSEBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
	btn.button.clicks = 1;
	btn.button.x = x;
	btn.button.y = y;
	SDL_PushEvent(&btn);
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_pointer_down(int x, int y)
{
	PushSynthMouseAt(SDL_MOUSEBUTTONDOWN, x, y);
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_pointer_up(int x, int y)
{
	PushSynthMouseAt(SDL_MOUSEBUTTONUP, x, y);
}

// Touch-hint overlay: faint translucent regions over the canvas showing
// where to tap for flippers / plunger. Called from RenderUi. Auto-hides
// after FadeSeconds, and gets dismissed instantly by the first touch
static bool ClientIsCoarsePointer()
{
	static int cached = -1; // -1 = not checked, 0 = false, 1 = true
	if (cached < 0)
		cached = EM_ASM_INT({
			return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0;
		});
	return cached == 1;
}

static void RenderTouchHints()
{
	if (!ClientIsCoarsePointer())
		return;

	using winmainClock = winmain::Clock;
	static auto firstShown = winmainClock::now();
	static bool initialized = false;
	if (!initialized) { firstShown = winmainClock::now(); initialized = true; }
	const auto elapsed = std::chrono::duration<float>(winmainClock::now() - firstShown).count();
	constexpr float FadeSeconds  = 7.0f;
	constexpr float FadeOutStart = 5.0f;
	if (elapsed >= FadeSeconds)
	{
		// Time-out: persist the dismiss the same way a touch would.
		options::Options.ShowTouchHints = false;
		return;
	}
	float alpha = 1.0f;
	if (elapsed > FadeOutStart)
		alpha = 1.0f - (elapsed - FadeOutStart) / (FadeSeconds - FadeOutStart);

	auto* dl = ImGui::GetForegroundDrawList();
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const float W = size.x, H = size.y;

	auto rgba = [alpha](float a) {
		return IM_COL32(255, 255, 255, static_cast<int>(a * alpha * 255));
	};
	const ImU32 fill   = rgba(0.10f);
	const ImU32 border = rgba(0.40f);
	const ImU32 text   = rgba(0.95f);

	// Same regions as actionForTouch() in emscripten_shell.html. Keep them
	// in sync if either is changed.
	const float plungerX0 = 0.30f * W, plungerX1 = 0.70f * W;
	const float plungerY0 = 0.80f * H;
	const float leftX0 = 0.00f, leftX1 = 0.50f * W;
	const float rightX0 = 0.50f * W, rightX1 = W;
	const float flipperY0 = 0.55f * H; // visual hint only, touch is whole half

	auto label = [&](const char* s, ImVec2 center) {
		const ImVec2 ts = ImGui::CalcTextSize(s);
		dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), text, s);
	};

	// Left flipper
	dl->AddRectFilled(ImVec2(leftX0, flipperY0), ImVec2(leftX1, H), fill, 8.0f);
	dl->AddRect      (ImVec2(leftX0, flipperY0), ImVec2(leftX1, H), border, 8.0f, 0, 2.0f);
	label("Left Flipper", ImVec2((leftX0 + leftX1) * 0.5f, (flipperY0 + H) * 0.5f));

	// Right flipper
	dl->AddRectFilled(ImVec2(rightX0, flipperY0), ImVec2(rightX1, H), fill, 8.0f);
	dl->AddRect      (ImVec2(rightX0, flipperY0), ImVec2(rightX1, H), border, 8.0f, 0, 2.0f);
	label("Right Flipper", ImVec2((rightX0 + rightX1) * 0.5f, (flipperY0 + H) * 0.5f));

	// Plunger band (drawn on top of the flipper halves)
	dl->AddRectFilled(ImVec2(plungerX0, plungerY0), ImVec2(plungerX1, H), fill, 8.0f);
	dl->AddRect      (ImVec2(plungerX0, plungerY0), ImVec2(plungerX1, H), border, 8.0f, 0, 2.0f);
	label("Hold to pull plunger", ImVec2((plungerX0 + plungerX1) * 0.5f, (plungerY0 + H) * 0.5f));

	// Caption near the top. ASCII only, since ProggyClean (default ImGui font) has
	// no em-dash glyph.
	const char* caption = "Touch to play, hints will fade";
	const ImVec2 ts = ImGui::CalcTextSize(caption);
	dl->AddText(ImVec2((W - ts.x) * 0.5f, H * 0.06f), text, caption);
}
void winmain::WebFlushPersistence()
{
	// Eagerly push state to disk: native does this at quit via pb::uninit /
	// options::uninit, but on the web the user just closes the tab.
	options::uninit();
	high_score::write();
	if (ImIO && ImIO->IniFilename)
		ImGui::SaveIniSettingsToDisk(ImIO->IniFilename);
	EM_ASM({
		if (Module.FS)
			Module.FS.syncfs(false, function (err) {
				if (err) console.error('IDBFS flush failed:', err);
			});
	});
}
#endif

int winmain::StartSession()
{
	restart = false;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	ImIO = &io;
	// s_iniPath is a file-static so io.IniFilename keeps a stable pointer
	// across the lifetime of the ImGui context.
	s_iniPath = std::string(s_prefPath) + "imgui_pb.ini";
	io.IniFilename = s_iniPath.c_str();

	// First option initialization step: just load settings from .ini. Needs ImGui context.
	options::InitPrimary();
	if (s_resetAllOptions)
	{
		s_resetAllOptions = false;
		options::ResetAllOptions();
	}

	if (!Options.FontFileName.V.empty())
	{
		ImVector<ImWchar> ranges;
		translations::GetGlyphRange(&ranges);
		ImFontConfig fontConfig{};

		fontConfig.OversampleV = 2;
		fontConfig.OversampleH = 4;

		auto fontLoaded = false;
		auto fileName = Options.FontFileName.V.c_str();
		auto fileHandle = fopenu(fileName, "rb");
		if (fileHandle)
		{
			fclose(fileHandle);
			if (io.Fonts->AddFontFromFileTTF(fileName, 13.f, &fontConfig, ranges.Data))
				fontLoaded = true;
		}

		if (!fontLoaded)
			printf("Failed to load font: %s, using embedded font.\n", fileName);
		io.Fonts->Build();
	}
	ImGui_Render_Init(Renderer);
	ImGui::StyleColorsDark();

	ImGui_ImplSDL2_InitForSDLRenderer(MainWindow, Renderer);
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

#ifdef __EMSCRIPTEN__
	// ?table=ft|3dpb URL override, applied once per page load before .dat
	// selection. Gated by s_overrideApplied so a later in-menu toggle
	// (which writes Prefer3DPBGameData to the ini) wins on subsequent
	// Restart()s.
	{
		static bool s_overrideApplied = false;
		if (!s_overrideApplied)
		{
			s_overrideApplied = true;
			int override_v = EM_ASM_INT({
				return (typeof window.__pinball_table_override === 'number')
				        ? window.__pinball_table_override : -1;
			});
			if (override_v == 0 || override_v == 1)
			{
				Options.Prefer3DPBGameData = (override_v == 1);
				Options.Resolution = -1; // boot at the chosen table's native max
				printf("URL ?table= override: %s\n", override_v ? "3D Pinball" : "Full Tilt");
			}
		}
	}
#endif

	// Data search order: WD, executable path, user pref path, platform specific paths.
	std::vector<const char*> searchPaths{{"", s_basePath, s_prefPath}};
	searchPaths.insert(searchPaths.end(), std::begin(PlatformDataPaths), std::end(PlatformDataPaths));
	pb::SelectDatFile(searchPaths);

	// Second step: run updates that depend on .DAT file selection
	options::InitSecondary();

	Sound::Init(s_mixOpened, Options.SoundChannels, Options.Sounds, Options.SoundVolume);
	if (!s_mixOpened)
		Options.Sounds = false;

	if (!midi::music_init(s_mixOpened, Options.MusicVolume))
		Options.Music = false;

	if (pb::init())
	{
		std::string message = "The .dat file is missing.\n"
			"Make sure that the game data is present in any of the following locations:\n";
		for (auto path : searchPaths)
		{
			if (path)
				message = message + (path[0] ? path : "working directory") + "\n";
		}
		pb::ShowMessageBox(SDL_MESSAGEBOX_ERROR, "Could not load game data", message.c_str());
		return 1;
	}

	fullscrn::init();

	pb::reset_table();
	pb::firsttime_setup();

	if (strstr(s_lpCmdLine, "-fullscreen"))
		Options.FullScreen = true;

	if (!Options.FullScreen)
	{
		auto resInfo = &fullscrn::resolution_array[fullscrn::GetResolution()];
		SDL_SetWindowSize(MainWindow, resInfo->TableWidth, resInfo->TableHeight);
	}
	SDL_ShowWindow(MainWindow);
	fullscrn::set_screen_mode(Options.FullScreen);

	if (strstr(s_lpCmdLine, "-demo"))
		pb::toggle_demo();
	else
		pb::replay_level(false);

	return 0;
}

void winmain::EndSession()
{
	options::uninit();
	midi::music_shutdown();
	Sound::Close();
	pb::uninit();

	ImGui_Render_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	ImIO = nullptr;
}

static unsigned ml_updateCounter, ml_frameCounter;
static winmain::TimePoint ml_frameStart, ml_prevTime;
static double ml_UpdateToFrameCounter;
static winmain::DurationMs ml_sleepRemainder, ml_frameDuration;
#ifdef __EMSCRIPTEN__
// Unspent real time carried between browser frames, fed to the fixed-timestep
// physics stepper so collisions resolve at a constant rate (see MainLoopIteration).
static double ml_physicsAccumulator;
// Wall-clock of last IDBFS flush. We push state every few seconds so a tab
// close doesn't lose progress.
static winmain::TimePoint ml_lastPersistFlush;
#endif

void winmain::MainLoop()
{
	bQuit = false;
	ml_updateCounter = 0;
	ml_frameCounter = 0;
	ml_frameStart = Clock::now();
	ml_UpdateToFrameCounter = 0;
	ml_sleepRemainder = DurationMs(0);
	ml_frameDuration = TargetFrameTime;
	ml_prevTime = ml_frameStart;
#ifdef __EMSCRIPTEN__
	ml_physicsAccumulator = 0;
	ml_lastPersistFlush = Clock::now();
#endif

#ifdef __EMSCRIPTEN__
	// 0 fps = browser drives via requestAnimationFrame.
	// simulate_infinite_loop=1 unwinds main() and yields control to the browser.
	emscripten_set_main_loop(MainLoopIteration, 0, 1);
#else
	while (!bQuit)
		MainLoopIteration();

	if (PrevSdlErrorCount > 0)
		printf("SDL Error: ^ Previous Error Repeated %u Times\n", PrevSdlErrorCount);
#endif
}

void winmain::MainLoopIteration()
{
	auto& updateCounter        = ml_updateCounter;
	auto& frameCounter         = ml_frameCounter;
	auto& frameStart           = ml_frameStart;
	auto& prevTime             = ml_prevTime;
	auto& frameDuration        = ml_frameDuration;
#ifndef __EMSCRIPTEN__
	// Native-only pacing state. The Emscripten path is rAF-paced (no sleep)
	// and renders every callback, so it needs neither of these.
	auto& UpdateToFrameCounter = ml_UpdateToFrameCounter;
	auto& sleepRemainder       = ml_sleepRemainder;
#endif

	{
		if (DispFrameRate)
		{
			auto curTime = Clock::now();
			if (curTime - prevTime > DurationMs(1000))
			{
				char buf[60];
				auto elapsedSec = DurationMs(curTime - prevTime).count() * 0.001;
				snprintf(buf, sizeof buf, "Updates/sec = %02.02f Frames/sec = %02.02f ",
				         updateCounter / elapsedSec, frameCounter / elapsedSec);
				SDL_SetWindowTitle(MainWindow, buf);
				FpsDetails = buf;
				frameCounter = updateCounter = 0;
				prevTime = curTime;
			}
		}

		if (!ProcessWindowMessages() || bQuit)
		{
#ifdef __EMSCRIPTEN__
			// On native, bQuit drops out of MainLoop() and the do-while in
			// WinMain handles restart vs exit. On web that loop can't unwind,
			// so restart is serviced inline: tear the session down, spin a
			// fresh one, and keep the rAF callback alive.
			if (restart)
			{
				EndSession();
				bQuit = false;
				StartSession();
				return;
			}
			emscripten_cancel_main_loop();
#endif
			return;
		}

#ifdef __EMSCRIPTEN__
		// Browser canvas has no real focus or minimised distinction. Force it
		// true so the game runs without needing a focus click. If
		// the tab is hidden, drop into a pause: skip physics + audio
		// and return early. This keeps a backgrounded tab off the CPU /
		// battery. rAF already throttles to ~1 Hz when hidden
		const bool tabVisible = EM_ASM_INT({ return document.hidden ? 0 : 1; });
		has_focus = tabVisible;
		if (!tabVisible)
		{
			// Reset frame timing so the next visible frame doesn't get a
			// huge dt (which would spike the physics accumulator and affect
			//gameplay)
			frameStart = Clock::now();
			frameDuration = TargetFrameTime;
			ml_physicsAccumulator = 0.0;
			return;
		}
#endif

		if (has_focus)
		{
			if (mouse_down)
			{
				int x, y, w, h;
				SDL_GetMouseState(&x, &y);
				SDL_GetWindowSize(MainWindow, &w, &h);
				float dx = static_cast<float>(last_mouse_x - x) / static_cast<float>(w);
				float dy = static_cast<float>(y - last_mouse_y) / static_cast<float>(h);
				pb::ballset(dx, dy);

				// Original creates continuous mouse movement with mouse capture.
				// Alternative solution: mouse warp at window edges.
				int xMod = 0, yMod = 0;
				if (x == 0 || x >= w - 1)
					xMod = w - 2;
				if (y == 0 || y >= h - 1)
					yMod = h - 2;
				if (xMod != 0 || yMod != 0)
				{
					// Mouse warp does not work over remote desktop or in some VMs
					x = abs(x - xMod);
					y = abs(y - yMod);
					SDL_WarpMouseInWindow(MainWindow, x, y);
				}

				last_mouse_x = x;
				last_mouse_y = y;
			}
			if (!single_step && !no_time_loss)
			{
#ifdef __EMSCRIPTEN__
				// The browser drives this loop at the display refresh rate, but
				// the physics is tuned for Options.UpdatesPerSecond. Step it at
				// the fixed TargetFrameTime, consuming whatever real time
				// elapsed, so a resting ball sees the same per-tick gravity and
				// collision response it would on the native build. Without this
				// resting balls micro-bounce.
				const double fixedStep = TargetFrameTime.count();
				ml_physicsAccumulator += frameDuration.count();
				int physicsSteps = 0;
				while (ml_physicsAccumulator >= fixedStep && physicsSteps < 8)
				{
					pb::frame(static_cast<float>(fixedStep));
					ml_physicsAccumulator -= fixedStep;
					physicsSteps++;
					updateCounter++;
				}
				// Fell behind (long stall), so drop the backlog instead of
				// triggering a spiral-of-death catch-up.
				if (physicsSteps == 8)
					ml_physicsAccumulator = 0.0;
				if (DispGRhistory)
				{
					auto targetSize = static_cast<unsigned>(static_cast<float>(Options.UpdatesPerSecond) * gfrWindow);
					if (gfrDisplay.size() != targetSize)
					{
						gfrDisplay.resize(targetSize, static_cast<float>(TargetFrameTime.count()));
						gfrOffset = 0;
					}
					gfrDisplay[gfrOffset] = static_cast<float>(fixedStep);
					gfrOffset = (gfrOffset + 1) % gfrDisplay.size();
				}
#else
				auto dt = static_cast<float>(frameDuration.count());
				pb::frame(dt);
				if (DispGRhistory)
				{
					auto targetSize = static_cast<unsigned>(static_cast<float>(Options.UpdatesPerSecond) * gfrWindow);
					if (gfrDisplay.size() != targetSize)
					{
						gfrDisplay.resize(targetSize, static_cast<float>(TargetFrameTime.count()));
						gfrOffset = 0;
					}
					gfrDisplay[gfrOffset] = dt;
					gfrOffset = (gfrOffset + 1) % gfrDisplay.size();
				}
				updateCounter++;
#endif
			}
			no_time_loss = false;

			// the browser already paces us at the display rate, so
			// render every callback
#ifdef __EMSCRIPTEN__
			{
#else
			if (UpdateToFrameCounter >= UpdateToFrameRatio)
			{
#endif
				if (Options.HideCursor && CursorIdleCounter <= 0)
					ImGui::SetMouseCursor(ImGuiMouseCursor_None);
				ImGui_ImplSDL2_NewFrame();
				ImGui_Render_NewFrame();
				ImGui::NewFrame();
				RenderUi();

				SDL_RenderClear(Renderer);
				// Alternative clear hack, clear might fail on some systems
				// Todo: remove original clear, if save for all platforms
				SDL_RenderFillRect(Renderer, nullptr);
				render::PresentVScreen();

				ImGui::Render();
				ImGui_Render_RenderDrawData(ImGui::GetDrawData());

				SDL_RenderPresent(Renderer);
				frameCounter++;
#ifndef __EMSCRIPTEN__
				UpdateToFrameCounter -= UpdateToFrameRatio;
#endif
			}

			auto sdlError = SDL_GetError();
			if (sdlError[0] || !PrevSdlError.empty())
			{
				if (sdlError[0])
					SDL_ClearError();

				// Rate limit duplicate SDL error messages.
				if (sdlError != PrevSdlError)
				{
					PrevSdlError = sdlError;
					if (PrevSdlErrorCount > 0)
					{
						printf("SDL Error: ^ Previous Error Repeated %u Times\n", PrevSdlErrorCount + 1);
						PrevSdlErrorCount = 0;
					}

					if (sdlError[0])
						printf("SDL Error: %s\n", sdlError);
				}
				else
				{
					PrevSdlErrorCount++;
				}
			}

#ifdef __EMSCRIPTEN__
			// requestAnimationFrame paces this loop, so never busy-wait.
			// Measure how much real time elapsed since the previous callback,
			// the physics stepper above turns it into fixed-size ticks. Clamp
			// so a long stall (tab backgrounded) can't dump a huge backlog.
			auto frameEnd = Clock::now();
			frameDuration = std::min<DurationMs>(DurationMs(frameEnd - frameStart), DurationMs(100));
			frameStart = frameEnd;

			if (DurationMs(frameEnd - ml_lastPersistFlush) >= DurationMs(5000))
			{
				ml_lastPersistFlush = frameEnd;
				WebFlushPersistence();
			}
#else
			auto updateEnd = Clock::now();
			auto targetTimeDelta = TargetFrameTime - DurationMs(updateEnd - frameStart) - sleepRemainder;

			TimePoint frameEnd;
			if (targetTimeDelta > DurationMs::zero() && !Options.UncappedUpdatesPerSecond)
			{
				if (Options.HybridSleep)
					HybridSleep(targetTimeDelta);
				else
					std::this_thread::sleep_for(targetTimeDelta);
				frameEnd = Clock::now();
			}
			else
			{
				frameEnd = updateEnd;
			}

			// Limit duration to 2 * target time
			sleepRemainder = Clamp(DurationMs(frameEnd - updateEnd) - targetTimeDelta, -TargetFrameTime,
			                       TargetFrameTime);
			frameDuration = std::min<DurationMs>(DurationMs(frameEnd - frameStart), 2 * TargetFrameTime);
			frameStart = frameEnd;
			UpdateToFrameCounter++;
#endif

			CursorIdleCounter = std::max(CursorIdleCounter - static_cast<int>(frameDuration.count()), 0);
		}
	}
}

void winmain::RenderUi()
{
	// Transparent menu bar with a button for preventing menu lockout.
	ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4{});
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	if (!Options.ShowMenu && ImGui::BeginMainMenuBar())
	{
		if (ImGui::MenuItem("Menu"))
		{
			options::toggle(Menu1::Show_Menu);
			ImGui::FocusWindow(nullptr);
		}
		ImGui::EndMainMenuBar();
	}
	ImGui::PopStyleVar(1);
	ImGui::PopStyleColor(2);

	// No demo window in release to save space
#ifndef NDEBUG
	if (ShowImGuiDemo)
		ImGui::ShowDemoWindow(&ShowImGuiDemo);
#endif

	if (Options.ShowMenu && ImGui::BeginMainMenuBar())
	{
		int currentMenuHeight = static_cast<int>(ImGui::GetWindowSize().y);
		if (MainMenuHeight != currentMenuHeight)
		{
			// Get the height of the main menu bar and update screen coordinates
			MainMenuHeight = currentMenuHeight;
			fullscrn::window_size_changed();
		}

		if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Game)))
		{
			ImGuiMenuItemWShortcut(GameBindings::NewGame);
			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_Launch_Ball), nullptr, false, LaunchBallEnabled))
			{
				end_pause();
				pb::launch_ball();
			}
			ImGuiMenuItemWShortcut(GameBindings::TogglePause);
			ImGui::Separator();

			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_High_Scores), nullptr, false, HighScoresEnabled))
			{
				pause(false);
				pb::high_scores();
			}
			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_Demo), nullptr, DemoActive))
			{
				end_pause();
				pb::toggle_demo();
			}
			ImGuiMenuItemWShortcut(GameBindings::Exit);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Options)))
		{
			ImGuiMenuItemWShortcut(GameBindings::ToggleMenuDisplay, Options.ShowMenu);
			ImGuiMenuItemWShortcut(GameBindings::ToggleFullScreen, Options.FullScreen);
			if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Select_Players)))
			{
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_1Player), nullptr, Options.Players == 1))
				{
					options::toggle(Menu1::OnePlayer);
					new_game();
				}
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_2Players), nullptr, Options.Players == 2))
				{
					options::toggle(Menu1::TwoPlayers);
					new_game();
				}
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_3Players), nullptr, Options.Players == 3))
				{
					options::toggle(Menu1::ThreePlayers);
					new_game();
				}
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_4Players), nullptr, Options.Players == 4))
				{
					options::toggle(Menu1::FourPlayers);
					new_game();
				}
				ImGui::EndMenu();
			}
			ImGuiMenuItemWShortcut(GameBindings::ShowControlDialog);
			if (ImGui::BeginMenu("Language"))
			{
				auto currentLanguage = translations::GetCurrentLanguage();
				for (auto& item : translations::Languages)
				{
					if (ImGui::MenuItem(item.DisplayName, nullptr, currentLanguage->Language == item.Language))
					{
						if (currentLanguage->Language != item.Language)
						{
							translations::SetCurrentLanguage(item.ShortName);
							Restart();
						}
					}
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();

			if (ImGui::BeginMenu("Audio"))
			{
				ImGuiMenuItemWShortcut(GameBindings::ToggleSounds, Options.Sounds);
				if (ImGui::MenuItem("Stereo Sound Effects", nullptr, Options.SoundStereo))
				{
					options::toggle(Menu1::SoundStereo);
				}
				ImGui::TextUnformatted("Sound Volume");
				if (ImGui::SliderInt("##Sound Volume", &Options.SoundVolume.V, options::MinVolume, options::MaxVolume,
				                     "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					Sound::SetVolume(Options.SoundVolume);
				}
				ImGui::TextUnformatted("Sound Channels");
				if (ImGui::SliderInt("##Sound Channels", &Options.SoundChannels.V, options::MinSoundChannels,
				                     options::MaxSoundChannels, "%d", ImGuiSliderFlags_AlwaysClamp))
				{
					Sound::SetChannels(Options.SoundChannels);
				}
				ImGui::Separator();

				ImGuiMenuItemWShortcut(GameBindings::ToggleMusic, Options.Music);
				ImGui::TextUnformatted("Music Volume");
				if (ImGui::SliderInt("##Music Volume", &Options.MusicVolume.V, options::MinVolume, options::MaxVolume,
				                     "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					midi::SetVolume(Options.MusicVolume);
				}
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Graphics"))
			{
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_WindowUniformScale), nullptr, Options.UniformScaling))
				{
					options::toggle(Menu1::WindowUniformScale);
				}
				if (ImGui::MenuItem("Linear Filtering", nullptr, Options.LinearFiltering))
				{
					options::toggle(Menu1::WindowLinearFilter);
				}
				if (ImGui::MenuItem("Integer Scaling", nullptr, Options.IntegerScaling))
				{
					options::toggle(Menu1::WindowIntegerScale);
				}
				if (ImGui::DragFloat("UI Scale", &Options.UIScale.V, 0.005f, 0.8f, 5,
				                     "%.2f", ImGuiSliderFlags_AlwaysClamp))
				{
					ImIO->FontGlobalScale = Options.UIScale;
				}
				ImGui::Separator();

				char buffer[80]{};
				auto changed = false;
				if (ImGui::MenuItem("Set Default UPS/FPS"))
				{
					changed = true;
					Options.UpdatesPerSecond = options::DefUps;
					Options.FramesPerSecond = options::DefFps;
				}
				if (ImGui::SliderInt("UPS", &Options.UpdatesPerSecond.V, options::MinUps, options::MaxUps, "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					changed = true;
					Options.FramesPerSecond = std::min(Options.UpdatesPerSecond.V, Options.FramesPerSecond.V);
				}
				if (ImGui::SliderInt("FPS", &Options.FramesPerSecond.V, options::MinFps, options::MaxFps, "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					changed = true;
					Options.UpdatesPerSecond = std::max(Options.UpdatesPerSecond.V, Options.FramesPerSecond.V);
				}
				snprintf(buffer, sizeof buffer - 1, "Uncapped UPS (FPS ratio %02.02f)", UpdateToFrameRatio);
				if (ImGui::MenuItem(buffer, nullptr, Options.UncappedUpdatesPerSecond))
				{
					Options.UncappedUpdatesPerSecond ^= true;
				}
				if (ImGui::MenuItem("Precise Sleep", nullptr, Options.HybridSleep))
				{
					Options.HybridSleep ^= true;
					SleepState = WelfordState{};
					SpinThreshold = DurationMs::zero();
				}

				if (changed)
				{
					UpdateFrameRate();
				}
				ImGui::Separator();

				if (ImGui::MenuItem("Hide Cursor", nullptr, Options.HideCursor))
				{
					Options.HideCursor ^= true;
				}
				if (ImGui::MenuItem("Change Font..."))
				{
					font_selection::ShowDialog();
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Table_Resolution)))
			{
				char buffer[20]{};
				auto resolutionStringId = Msg::Menu1_UseMaxResolution_640x480;

				switch (fullscrn::GetMaxResolution())
				{
				case 0: resolutionStringId = Msg::Menu1_UseMaxResolution_640x480;
					break;
				case 1: resolutionStringId = Msg::Menu1_UseMaxResolution_800x600;
					break;
				case 2: resolutionStringId = Msg::Menu1_UseMaxResolution_1024x768;
					break;
				}

				auto maxResText = pb::get_rc_string(resolutionStringId);
				if (ImGui::MenuItem(maxResText, nullptr, Options.Resolution == -1))
				{
					options::toggle(Menu1::MaximumResolution);
				}
				for (auto i = 0; i <= fullscrn::GetMaxResolution(); i++)
				{
					auto& res = fullscrn::resolution_array[i];
					snprintf(buffer, sizeof buffer - 1, "%d x %d", res.ScreenWidth, res.ScreenHeight);
					if (ImGui::MenuItem(buffer, nullptr, Options.Resolution == i))
					{
						options::toggle(static_cast<Menu1>(static_cast<int>(Menu1::R640x480) + i));
					}
				}
				ImGui::EndMenu();
			}

			ImGui::Separator();
			if (ImGui::MenuItem("Reset All Options"))
			{
				options::ResetAllOptions();
				Restart();
			}
			ImGui::EndMenu();
		}

		// Top-level "Table" menu, lets the user switch between the two
		// bundled pinball tables at a glance. The underlying option
		// (Prefer3DPBGameData) just biases the .DAT search order: false →
		// CADET.DAT first (Full Tilt), true → PINBALL.DAT first (3DPB).
		// Restart happens via options::toggle. The rAF callback re-runs
		// session lifecycle inline.
		if (ImGui::BeginMenu("Table"))
		{
			const bool useFt   = !Options.Prefer3DPBGameData;
			const bool use3dpb =  Options.Prefer3DPBGameData;

			if (ImGui::MenuItem("Full Tilt - Space Cadet", nullptr, useFt))
			{
				if (!useFt) options::toggle(Menu1::Prefer3DPBGameData);
			}
			if (ImGui::MenuItem("3D Pinball (Microsoft Plus!)", nullptr, use3dpb))
			{
				if (!use3dpb) options::toggle(Menu1::Prefer3DPBGameData);
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Help)))
		{
#ifndef NDEBUG
			if (ImGui::MenuItem("ImGui Demo", nullptr, ShowImGuiDemo))
			{
				ShowImGuiDemo ^= true;
			}
#endif
			if (ImGui::MenuItem("Sprite Viewer", nullptr, ShowSpriteViewer))
			{
				if (!ShowSpriteViewer)
					pause(false);
				ShowSpriteViewer ^= true;
			}
			if (pb::cheat_mode && ImGui::MenuItem("Frame Times", nullptr, DispGRhistory))
			{
				DispGRhistory ^= true;
			}
			if (ImGui::MenuItem("Debug Overlay", nullptr, Options.DebugOverlay))
			{
				Options.DebugOverlay ^= true;
			}
			if (Options.DebugOverlay && ImGui::BeginMenu("Overlay Options"))
			{
				if (ImGui::MenuItem("Box Grid", nullptr, Options.DebugOverlayGrid))
					Options.DebugOverlayGrid ^= true;
				if (ImGui::MenuItem("Ball Depth Grid", nullptr, Options.DebugOverlayBallDepthGrid))
					Options.DebugOverlayBallDepthGrid ^= true;
				if (ImGui::MenuItem("Sprite Positions", nullptr, Options.DebugOverlaySprites))
					Options.DebugOverlaySprites ^= true;
				if (ImGui::MenuItem("All Edges", nullptr, Options.DebugOverlayAllEdges))
					Options.DebugOverlayAllEdges ^= true;
				if (ImGui::MenuItem("Component AABB", nullptr, Options.DebugOverlayAabb))
					Options.DebugOverlayAabb ^= true;
				if (ImGui::MenuItem("Ball Position", nullptr, Options.DebugOverlayBallPosition))
					Options.DebugOverlayBallPosition ^= true;
				if (ImGui::MenuItem("Ball Box Edges", nullptr, Options.DebugOverlayBallEdges))
					Options.DebugOverlayBallEdges ^= true;
				if (ImGui::MenuItem("Sound Positions", nullptr, Options.DebugOverlaySounds))
					Options.DebugOverlaySounds ^= true;
				if (ImGui::MenuItem("Apply Collision Mask", nullptr, Options.DebugOverlayCollisionMask))
					Options.DebugOverlayCollisionMask ^= true;
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Cheats"))
			{
				if (ImGui::MenuItem("hidden test", nullptr, pb::cheat_mode))
					pb::PushCheat("hidden test");
				if (ImGui::MenuItem("1max"))
					pb::PushCheat("1max");
				if (ImGui::MenuItem("bmax", nullptr, control::table_unlimited_balls))
					pb::PushCheat("bmax");
				if (ImGui::MenuItem("gmax"))
					pb::PushCheat("gmax");
				if (ImGui::MenuItem("rmax"))
					pb::PushCheat("rmax");
				if (pb::FullTiltMode && ImGui::MenuItem("quote"))
					pb::PushCheat("quote");
				if (ImGui::MenuItem("easy mode", nullptr, control::easyMode))
					pb::PushCheat("easy mode");

				ImGui::EndMenu();
			}
			ImGui::Separator();

			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_About_Pinball)))
			{
				pause(false);
				ShowAboutDialog = true;
			}
			ImGui::EndMenu();
		}
		if (DispFrameRate && !FpsDetails.empty())
			if (ImGui::BeginMenu(FpsDetails.c_str()))
				ImGui::EndMenu();
		ImGui::EndMainMenuBar();
	}

#ifdef __EMSCRIPTEN__
	// One-shot touch tutorial, see RenderTouchHints definition below.
	if (Options.ShowTouchHints)
		RenderTouchHints();
#endif

	a_dialog();
	high_score::RenderHighScoreDialog();
	font_selection::RenderDialog();
	if (ShowSpriteViewer)
		render::SpriteViewer(&ShowSpriteViewer);
	options::RenderControlDialog();
	if (DispGRhistory)
		RenderFrameTimeDialog();

	const auto exitText = translations::GetTranslation(Msg::Menu1_Exit);
	if (ShowExitPopup)
	{
		ShowExitPopup = false;
		pause(false);
		ImGui::OpenPopup(exitText);
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	}
	if (ImGui::BeginPopupModal(exitText, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Exit the game?");
		ImGui::Separator();

		if (ImGui::Button(pb::get_rc_string(Msg::GenericOk), ImVec2(120, 0)))
		{
			SDL_Event event{SDL_QUIT};
			SDL_PushEvent(&event);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere(0);
		}
		if (ImGui::Button(pb::get_rc_string(Msg::GenericCancel), ImVec2(120, 0)))
		{
			end_pause();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::EndPopup();
	}

	// Print game texts on the sidebar
	gdrv::grtext_draw_ttext_in_box();
}

int winmain::event_handler(const SDL_Event* event)
{
	auto inputDown = false;
	switch (event->type)
	{
	case SDL_KEYDOWN:
	case SDL_MOUSEBUTTONDOWN:
	case SDL_CONTROLLERBUTTONDOWN:
		inputDown = true;
		break;
	default: break;
	}
	if (!options::WaitingForInput() || !inputDown)
		ImGui_ImplSDL2_ProcessEvent(event);

	bool mouseEvent;
	switch (event->type)
	{
	case SDL_MOUSEMOTION:
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEWHEEL:
		CursorIdleCounter = 1000;
		mouseEvent = true;
		break;
	default:
		mouseEvent = false;
		break;
	}

	if (ImIO->WantCaptureMouse && !options::WaitingForInput())
	{
		if (mouse_down)
		{
			mouse_down = 0;
			SDL_SetWindowGrab(MainWindow, SDL_FALSE);
		}
		if (mouseEvent)
			return 1;
	}
	if (ImIO->WantCaptureKeyboard && !options::WaitingForInput())
	{
		switch (event->type)
		{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			return 1;
		default: ;
		}
	}

	switch (event->type)
	{
	case SDL_QUIT:
		end_pause();
		bQuit = true;
		fullscrn::shutdown();
		return_value = 0;
		return 0;
	case SDL_KEYUP:
		// Use scancode (physical key) so bindings work on non-US layouts.
		pb::InputUp({InputTypes::Keyboard, event->key.keysym.scancode});
		break;
	case SDL_KEYDOWN:
		if (event->key.repeat)
			break;

		// Bind on scancode, and pass keysym separately for typed-letter cheats.
		pb::InputDown({InputTypes::Keyboard, event->key.keysym.scancode}, event->key.keysym.sym);
		if (!pb::cheat_mode)
			break;

		switch (event->key.keysym.sym)
		{
		case SDLK_g:
			DispGRhistory ^= true;
			break;
		case SDLK_o:
			{
				auto plt = new ColorRgba[4 * 256];
				auto pltPtr = &plt[10]; // first 10 entries are system colors hardcoded in display_palette()
				for (int i1 = 0, i2 = 0; i1 < 256 - 10; ++i1, i2 += 8)
				{
					unsigned char blue = i2, redGreen = i2;
					if (i2 > 255)
					{
						blue = 255;
						redGreen = i1;
					}

					*pltPtr++ = ColorRgba{blue, redGreen, redGreen, 0};
				}
				gdrv::display_palette(plt);
				delete[] plt;
			}
			break;
		case SDLK_y:
			SDL_SetWindowTitle(MainWindow, "Pinball");
			DispFrameRate ^= true;
			break;
		case SDLK_F1:
			pb::frame(10);
			break;
		case SDLK_F10:
			single_step ^= true;
			if (!single_step)
				no_time_loss = true;
			break;
		default:
			break;
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
		{
			bool noInput = false;
			switch (event->button.button)
			{
			case SDL_BUTTON_LEFT:
				if (pb::cheat_mode)
				{
					mouse_down = 1;
					last_mouse_x = event->button.x;
					last_mouse_y = event->button.y;
					SDL_SetWindowGrab(MainWindow, SDL_TRUE);
					noInput = true;
				}
				break;
			default:
				break;
			}

			if (!noInput)
				pb::InputDown({InputTypes::Mouse, event->button.button});
		}
		break;
	case SDL_MOUSEBUTTONUP:
		{
			bool noInput = false;
			switch (event->button.button)
			{
			case SDL_BUTTON_LEFT:
				if (mouse_down)
				{
					mouse_down = 0;
					SDL_SetWindowGrab(MainWindow, SDL_FALSE);
					noInput = true;
				}
				break;
			default:
				break;
			}

			if (!noInput)
				pb::InputUp({InputTypes::Mouse, event->button.button});
		}
		break;
	case SDL_WINDOWEVENT:
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_FOCUS_GAINED:
		case SDL_WINDOWEVENT_TAKE_FOCUS:
		case SDL_WINDOWEVENT_SHOWN:
			activated = true;
			Sound::Activate();
			if (Options.Music && !single_step)
				midi::music_play();
			no_time_loss = true;
			has_focus = true;
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
		case SDL_WINDOWEVENT_HIDDEN:
			activated = false;
			fullscrn::activate(0);
			Options.FullScreen = false;
			Sound::Deactivate();
			midi::music_stop();
			has_focus = false;
			pb::loose_focus();
			break;
		case SDL_WINDOWEVENT_SIZE_CHANGED:
		case SDL_WINDOWEVENT_RESIZED:
			fullscrn::window_size_changed();
			break;
		default: ;
		}
		break;
	case SDL_JOYDEVICEADDED:
		if (SDL_IsGameController(event->jdevice.which))
		{
			SDL_GameControllerOpen(event->jdevice.which);
		}
		break;
	case SDL_JOYDEVICEREMOVED:
		{
			SDL_GameController* controller = SDL_GameControllerFromInstanceID(event->jdevice.which);
			if (controller)
			{
				SDL_GameControllerClose(controller);
			}
		}
		break;
	case SDL_CONTROLLERBUTTONDOWN:
		pb::InputDown({InputTypes::GameController, event->cbutton.button});
		break;
	case SDL_CONTROLLERBUTTONUP:
		pb::InputUp({InputTypes::GameController, event->cbutton.button});
		break;
	default: ;
	}

	return 1;
}

int winmain::ProcessWindowMessages()
{
	static auto idleWait = 0;
	SDL_Event event;
#ifdef __EMSCRIPTEN__
	// On the web, the canvas is always "shown", there's no minimised state,
	// and SDL_WaitEventTimeout would block the browser thread. Always poll.
	{
#else
	if (has_focus)
	{
#endif
		idleWait = static_cast<int>(TargetFrameTime.count());
		while (SDL_PollEvent(&event))
		{
			if (!event_handler(&event))
				return 0;
		}

		return 1;
	}

	// Progressively wait longer when transitioning to idle
	idleWait = std::min(idleWait + static_cast<int>(TargetFrameTime.count()), 500);
	if (SDL_WaitEventTimeout(&event, idleWait))
	{
		idleWait = static_cast<int>(TargetFrameTime.count());
		return event_handler(&event);
	}
	return 1;
}

void winmain::memalloc_failure()
{
	midi::music_stop();
	Sound::Close();
	const char* caption = pb::get_rc_string(Msg::STRING270);
	const char* text = pb::get_rc_string(Msg::STRING279);
	pb::ShowMessageBox(SDL_MESSAGEBOX_ERROR, caption, text);
	std::exit(1);
}

void winmain::a_dialog()
{
	if (ShowAboutDialog == true)
	{
		ShowAboutDialog = false;
		ImGui::OpenPopup(pb::get_rc_string(Msg::STRING204));
	}

	bool unused_open = true;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{ 600, 300 });
	if (ImGui::BeginPopupModal(pb::get_rc_string(Msg::STRING204), &unused_open, ImGuiWindowFlags_None))
	{
		if (ImGui::BeginTabBar("AboutTabBar", ImGuiTabBarFlags_None))
		{
			if (ImGui::BeginTabItem("3DPB"))
			{
				ImGui::TextUnformatted(pb::get_rc_string(Msg::STRING139));
				ImGui::TextUnformatted("Original game by Cinematronics, Microsoft");
				ImGui::Separator();

				ImGui::TextUnformatted("Decompiled -> Ported to SDL");
				ImGui::Text("Version %s", Version);
				if (ImGui::SmallButton("Upstream decomp: https://github.com/k4zmu2a/SpaceCadetPinball"))
				{
#if SDL_VERSION_ATLEAST(2, 0, 14)
					SDL_OpenURL("https://github.com/k4zmu2a/SpaceCadetPinball");
#endif
				}
				ImGui::Separator();
				ImGui::TextUnformatted("This web build is based on both the original");
				ImGui::TextUnformatted("decomp and the alula Emscripten fork.");
				if (ImGui::SmallButton("alula fork: https://github.com/alula/SpaceCadetPinball"))
				{
#if SDL_VERSION_ATLEAST(2, 0, 14)
					SDL_OpenURL("https://github.com/alula/SpaceCadetPinball");
#endif
				}
				ImGui::TextUnformatted("Source code hosted on:");
				if (ImGui::SmallButton("https://github.com/omediodomonte38/SpaceCadetPinball"))
				{
#if SDL_VERSION_ATLEAST(2, 0, 14)
					SDL_OpenURL("https://github.com/omediodomonte38/SpaceCadetPinball");
#endif
				}
				ImGui::EndTabItem();
			}
			ImGui::PushStyleColor(ImGuiCol_Button, 0);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, 0);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, 0);
			if (ImGui::BeginTabItem("Full Tilt!"))
			{
				const ImVec2 buttonCenter = { -1, 0 };
				ImGui::Button("Full Tilt! was created by Cinematronics for Maxis.", buttonCenter);
				ImGui::Button("Version 1.1", buttonCenter);

				auto tableRow = [](LPCSTR textA, LPCSTR textB)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(textA);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(textB);
				};
				if (ImGui::BeginTable("Full Tilt!", 2))
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Button("Cinematronics", buttonCenter);
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("PROGRAMMING", "ART");
						tableRow("Michael Sandige", "John Frantz");
						tableRow("John Taylor", "Ryan Medeiros");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("DESIGN", "SOUND EFFECTS");
						tableRow("Kevin Gliner", "Matt Ridgeway");
						tableRow(nullptr, "Donald S. Griffin");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("DESIGN CONSULTANT", "MUSIC");
						tableRow("Mark Sprenger", "Matt Ridgeway");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("PRODUCER", "VOICES");
						tableRow("Kevin Gliner", "Mike McGeary");
						tableRow(nullptr, "William Rice");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("GRAND POOBAH", nullptr);
						tableRow("David Stafford", nullptr);
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::Button("SPECIAL THANKS", buttonCenter);
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("Paula Sandige", "Alex St. John");
						tableRow("Brad Silverberg", "Jeff Camp");
						tableRow("Danny Thorpe", "Greg Hospelhorn");
						tableRow("Keith Johnson", "Sean Grant");
						tableRow("Bob McAnn", "Michael Kelley");
						tableRow("Rob Rosenhouse", "Lisa Acton");
						ImGui::EndTable();
					}
					ImGui::TextUnformatted("Dan and Mitchell Roth");

					ImGui::TableNextColumn();
					ImGui::Button("Maxis", buttonCenter);
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("PRODUCER", "PRODUCT MANAGER");
						tableRow("John Csicsery", "Larry Lee");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("LEAD TESTER", "QA MANAGER");
						tableRow("Scott Shicoff", "Scott Shicoff");
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::Button("ADDITIONAL TESTING", buttonCenter);
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("Cathy Castro", "Robin Hines");
						tableRow("John \"Jussi\" Ylinen", "Keith Meyer");
						tableRow("Marc Meyer", "Owen Nelson");
						tableRow("Joe Longworth", "Peter Saylor");
						tableRow("Michael Gilmartin", "Robin Hines");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("ADDITIONAL ART", "ART DIRECTOR");
						tableRow("Ocean Quigley", "Sharon Barr");
						tableRow("Rick Macaraeg", "INSTALL PROGRAM");
						tableRow("Charlie Aquilina", "Kevin O'Hare");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("INTRO MUSIC", "DOCUMENTATION");
						tableRow("Brian Conrad", "David Caggiano");
						tableRow("John Csicsery", "Michael Bremer");
						tableRow(nullptr, "Bob Sombrio");
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::Button("SPECIAL THANKS", buttonCenter);
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("Sam Poole", "Joe Scirica");
						tableRow("Jeff Braun", "Bob Derber");
						tableRow("Ashley Csicsery", "Tom Forge");
						ImGui::EndTable();
					}
					ImGui::Button("Will \"Burr\" Wright", buttonCenter);
					ImGui::EndTable();
				}

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
			ImGui::PopStyleColor(3);
		}

		ImGui::Separator();
		if (ImGui::Button("Ok"))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

void winmain::end_pause()
{
	if (single_step)
	{
		pb::pause_continue();
		no_time_loss = true;
	}
}

void winmain::new_game()
{
	end_pause();
	pb::replay_level(false);
}

void winmain::pause(bool toggle)
{
	if (toggle || !single_step)
	{
		pb::pause_continue();
		no_time_loss = true;
	}
}

void winmain::Restart()
{
	restart = true;
	SDL_Event event{SDL_QUIT};
	SDL_PushEvent(&event);
}

void winmain::UpdateFrameRate()
{
	// UPS >= FPS
	auto fps = Options.FramesPerSecond.V, ups = Options.UpdatesPerSecond.V;
	UpdateToFrameRatio = static_cast<double>(ups) / fps;
	TargetFrameTime = DurationMs(1000.0 / ups);
}

void winmain::HandleGameBinding(GameBindings binding, bool shortcut)
{
	switch (binding)
	{
	case GameBindings::TogglePause:
		pause();
		break;
	case GameBindings::NewGame:
		new_game();
		break;
	case GameBindings::ToggleFullScreen:
		options::toggle(Menu1::Full_Screen);
		break;
	case GameBindings::ToggleSounds:
		options::toggle(Menu1::Sounds);
		break;
	case GameBindings::ToggleMusic:
		options::toggle(Menu1::Music);
		break;
	case GameBindings::ShowControlDialog:
		pause(false);
		options::ShowControlDialog();
		break;
	case GameBindings::ToggleMenuDisplay:
		options::toggle(Menu1::Show_Menu);
		break;
	case GameBindings::Exit:
		if (!shortcut)
		{
			SDL_Event event{SDL_QUIT};
			SDL_PushEvent(&event);
		}
		else
			ShowExitPopup = true;
		break;
	default:
		break;
	}
}

void winmain::RenderFrameTimeDialog()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{300, 70});
	if (ImGui::Begin("Frame Times", &DispGRhistory, ImGuiWindowFlags_NoScrollbar))
	{
		auto target = static_cast<float>(TargetFrameTime.count());
		auto yMax = target * 2;

		auto spin = Options.HybridSleep ? static_cast<float>(SpinThreshold.count()) : 0;
		ImGui::Text("YMin:0ms, Target frame time:%03.04fms, YMax:%03.04fms, SpinThreshold:%03.04fms",
		            target, yMax, spin);

		static bool scrollPlot = true;
		ImGui::Checkbox("Scroll Plot", &scrollPlot);

		ImGui::SameLine();
		ImGui::SliderFloat("Window Size", &gfrWindow, 0.1f, 15, "%.3fsec", ImGuiSliderFlags_AlwaysClamp);

		{
			float average = 0.0f, dev = 0.0f;
			for (auto n : gfrDisplay)
			{
				average += n;
				dev += std::abs(target - n);
			}
			average /= static_cast<float>(gfrDisplay.size());
			dev /= static_cast<float>(gfrDisplay.size());
			char overlay[64];
			sprintf(overlay, "avg %.3fms, dev %.3fms", average, dev);

			auto region = ImGui::GetContentRegionAvail();
			ImGui::PlotLines("Lines", gfrDisplay.data(), static_cast<int>(gfrDisplay.size()),
			                 scrollPlot ? gfrOffset : 0, overlay, 0, yMax, region);
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void winmain::HybridSleep(DurationMs sleepTarget)
{
	static constexpr double StdDevFactor = 0.5;

	// This nice concept is from https://blat-blatnik.github.io/computerBear/making-accurate-sleep-function/
	// Sacrifices some CPU time for smaller frame time jitter
	while (sleepTarget > SpinThreshold)
	{
		auto start = Clock::now();
		std::this_thread::sleep_for(DurationMs(1));
		auto end = Clock::now();

		auto actualDuration = DurationMs(end - start);
		sleepTarget -= actualDuration;

		// Update expected sleep duration using Welford's online algorithm
		// With bad timer, this will run away to 100% spin
		SleepState.Advance(actualDuration.count());
		SpinThreshold = DurationMs(SleepState.mean + SleepState.GetStdDev() * StdDevFactor);
	}

	// spin lock
	for (auto start = Clock::now(); DurationMs(Clock::now() - start) < sleepTarget;);
}

void winmain::ImGuiMenuItemWShortcut(GameBindings binding, bool selected)
{
	const auto& keyDef = Options.Key[~binding];
	if (ImGui::MenuItem(pb::get_rc_string(keyDef.Description), keyDef.GetShortcutDescription().c_str(), selected))
	{
		HandleGameBinding(binding, false);
	}
}
