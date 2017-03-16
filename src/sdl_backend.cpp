#include "common.h"

#include "audio.h"
#include "cpu.h"
#include "input.h"
#ifdef RECORD_MOVIE
#  include "movie.h"
#endif
#include "save_states.h"
#include "sdl_backend.h"
#ifdef RUN_TESTS
#  include "test.h"
#endif

#include "dbgfont.xpm"

#include <SDL.h>
#include <SDL_image.h>
#include <stdarg.h>

//
// Video
//

// Each pixel is scaled to scale_factor*scale_factor pixels
unsigned const scale_factor = 2;

static SDL_Window   *screen;
static SDL_Renderer *renderer;
static SDL_Texture  *screen_tex;

static SDL_Texture  *dbg_font;

#define DEFAULT_W 640
#define DEFAULT_H 480

int win_w = DEFAULT_W;
int win_h = DEFAULT_H;
SDL_Rect viewport = {.x = 0, .y = 0, .w = DEFAULT_W, .h = DEFAULT_H};

SDL_Rect boxify() {

	double ratio = (win_w * 0.75) / win_h;

	int true_w = (ratio >= 1) ? win_h / 0.75 : win_w;
	int true_h = (ratio >= 1) ? win_h : win_w * 0.75;

	viewport = (SDL_Rect){.x = (win_w - true_w) / 2, .y = (win_h - true_h) / 2, .w = true_w, .h = true_h};
	return viewport;
}

// On Unity with the Nouveau driver, displaying the frame sometimes blocks for
// a very long time (to the tune of only managing 30 FPS with everything
// removed but render calls when the translucent Ubuntu menu is open, and often
// less than 60 with Firefox open too). This in turn slows down emulation and
// messes up audio. To get around it, we upload frames in the SDL thread and
// keep a back buffer for drawing into from the emulation thread while the
// frame is being uploaded (a kind of manual triple buffering). If the frame
// doesn't upload in time for the next frame, we drop the new frame. This gives
// us automatic frame skipping in general.
//
// TODO: This could probably be optimized to eliminate some copying and format
// conversions.

static Uint32 *front_buffer;
static Uint32 *back_buffer;

static SDL_mutex *frame_lock;
static SDL_cond  *frame_available_cond;
static bool ready_to_draw_new_frame;
static bool frame_available;

static bool show_debugger;

Uint8 debug_contents[128 * 60]; //640x480 / 5x8 font
Uint8 debug_colors[128*60];
Uint8 debug_cur_color = 0, debug_cur_x=0, debug_cur_y = 0;

char* cur_textinput;
int cur_textinput_sz;

void put_pixel(unsigned x, unsigned y, uint32_t color) {
	assert(x < 256);
	assert(y < 240);

	back_buffer[256*y + x] = color;
}

void draw_frame() {
#ifdef RECORD_MOVIE
	add_movie_video_frame(back_buffer);
#endif

	// Signal to the SDL thread that the frame has ended

	SDL_LockMutex(frame_lock);
	// Drop the new frame if the old one is still being rendered. This also
	// means that we drop event processing for one frame, but it's probably not
	// a huge deal.
	if (ready_to_draw_new_frame) {
		frame_available = true;
		swap(back_buffer, front_buffer);
		SDL_CondSignal(frame_available_cond);
	}
	SDL_UnlockMutex(frame_lock);
}

//
// Audio
//

Uint16 const sdl_audio_buffer_size = 2048;
static SDL_AudioDeviceID audio_device_id;

static void audio_callback(void*, Uint8 *stream, int len) {
	assert(len >= 0);

	read_samples((int16_t*)stream, len/sizeof(int16_t));
}

void lock_audio() { SDL_LockAudioDevice(audio_device_id); }
void unlock_audio() { SDL_UnlockAudioDevice(audio_device_id); }

void start_audio_playback() { SDL_PauseAudioDevice(audio_device_id, 0); }
void stop_audio_playback() { SDL_PauseAudioDevice(audio_device_id, 1); }

//
// Input
//

Uint8 const *keys;

Uint8 *keys_lf = NULL;
int keys_size = 0;

#define KEY_PRESSED(i) ( (keys[i]) & (!keys_lf[i]) )
#define KEY_RELEASED(i) ( (!keys[i]) & (keys_lf[i]) )

//
// SDL thread and events
//

bool cc_held = 0;

// Runs from emulation thread
void handle_ui_keys() {
	SDL_LockMutex(event_lock);

	if (keys[SDL_SCANCODE_ESCAPE])
		exit(0);


	if (KEY_PRESSED(SDL_SCANCODE_F3)) {
		corrupt_chance += 0x1000; printf("New corrupt chance is %u\n", corrupt_chance); }

	if (KEY_PRESSED(SDL_SCANCODE_F4)) {
		corrupt_chance -= 0x1000; printf("New corrupt chance is %u\n", corrupt_chance); }

	if (keys[SDL_SCANCODE_LALT] && (KEY_PRESSED(SDL_SCANCODE_D))) {
		show_debugger = !show_debugger;
	}

	if (keys[SDL_SCANCODE_F5])
		save_state();
	else if (keys[SDL_SCANCODE_F8])
		load_state();

	handle_rewind(keys[SDL_SCANCODE_BACKSPACE]);

	if (reset_pushed)
		soft_reset();

	SDL_UnlockMutex(event_lock);
	if (keys_size) memcpy(keys_lf, keys, keys_size * sizeof(Uint8));
}

static bool pending_sdl_thread_exit;

// Protects the 'keys' array from being read while being updated
SDL_mutex   *event_lock;

static void process_events() {
	SDL_Event event;
	SDL_LockMutex(event_lock);
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_WINDOWEVENT) {

			if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {

				SDL_GetWindowSize(screen,&win_w,&win_h);
				boxify();
			}

		} else if (event.type == SDL_QUIT) {
			end_emulation();
			pending_sdl_thread_exit = true;
#ifdef RUN_TESTS
			end_testing = true;
#endif
		}
	}
	SDL_UnlockMutex(event_lock);
}

const SDL_Rect screentex_valid = {.x = 12, .y = 0, .w = 256, .h = 240};

void sdl_thread() {
	for (;;) {

		// Wait for the emulation thread to signal that a frame has completed

		SDL_LockMutex(frame_lock);
		ready_to_draw_new_frame = true;
		while (!frame_available && !pending_sdl_thread_exit)
			SDL_CondWait(frame_available_cond, frame_lock);
		if (pending_sdl_thread_exit) {
			SDL_UnlockMutex(frame_lock);
			return;
		}
		frame_available = ready_to_draw_new_frame = false;
		SDL_UnlockMutex(frame_lock);

		// Process events and calculate controller input state (which might
		// need left+right/up+down elimination)

		process_events();

		// Draw the new frame


		fail_if(SDL_UpdateTexture(screen_tex, &screentex_valid, front_buffer, 256*sizeof(Uint32)),
				"failed to update screen texture: %s", SDL_GetError());
		fail_if(SDL_RenderCopy(renderer, screen_tex, 0, &viewport),
				"failed to copy rendered frame to render target: %s", SDL_GetError());

		if (show_debugger) {
			SDL_Rect dstrect;
			dstrect.w = 640; dstrect.h = 480;
			dstrect.x = win_w/2 - 320; dstrect.y = win_h/2 - 240;

			SDL_SetRenderDrawColor(renderer,0,0,0,128);
			SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
			SDL_RenderFillRect(renderer, &dstrect);
		
		SDL_Rect charrect; charrect.w = 5; charrect.h = 8;
		SDL_Rect dbgrect; dbgrect.w = 5; dbgrect.h = 8;

		for (int iy=0; iy < 60; iy++) {
			dbgrect.y = dstrect.y + (iy*8);
			for (int ix=0; ix < 128; ix++) {
				dbgrect.x = dstrect.x + (ix*5);
				if (debug_contents[iy*128+ix] >= 32) {
				charrect.x = ((debug_contents[iy*128+ix] - 32) % 16) * 5;
				charrect.y = ((debug_contents[iy*128+ix] - 32) / 16) * 8;
				
				fail_if(SDL_RenderCopy(renderer,dbg_font,&charrect,&dbgrect), "failed to draw debug character: %s",SDL_GetError());
				}
			}
		}
	

		}
		SDL_RenderPresent(renderer);
	}
}

void exit_sdl_thread() {
	SDL_LockMutex(frame_lock);
	pending_sdl_thread_exit = true;
	SDL_CondSignal(frame_available_cond);
	SDL_UnlockMutex(frame_lock);
}

//
// Initialization and de-initialization
//

void init_sdl() {
	SDL_version sdl_compiled_version, sdl_linked_version;
	SDL_VERSION(&sdl_compiled_version);
	SDL_GetVersion(&sdl_linked_version);
	printf("Using SDL backend. Compiled against SDL %d.%d.%d, linked to SDL %d.%d.%d.\n",
			sdl_compiled_version.major, sdl_compiled_version.minor, sdl_compiled_version.patch,
			sdl_linked_version.major, sdl_linked_version.minor, sdl_linked_version.patch);

	// SDL and video

	// Make this configurable later
	SDL_DisableScreenSaver();

	fail_if(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) != 0,
			"failed to initialize SDL: %s", SDL_GetError());

	fail_if(!(screen =
				SDL_CreateWindow(
					"Nesalizer",
					SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
					DEFAULT_W, DEFAULT_H,
					SDL_WINDOW_RESIZABLE)),
			"failed to create window: %s", SDL_GetError());

	fail_if(!(renderer = SDL_CreateRenderer(screen, -1, 0)),
			"failed to create rendering context: %s", SDL_GetError());
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

	// Display some information about the renderer
	SDL_RendererInfo renderer_info;
	if (SDL_GetRendererInfo(renderer, &renderer_info))
		puts("Failed to get renderer information from SDL");
	else {
		if (renderer_info.name)
			printf("renderer: uses renderer \"%s\"\n", renderer_info.name);
		if (renderer_info.flags & SDL_RENDERER_SOFTWARE)
			puts("renderer: uses software rendering");
		if (renderer_info.flags & SDL_RENDERER_ACCELERATED)
			puts("renderer: uses hardware-accelerated rendering");
		if (renderer_info.flags & SDL_RENDERER_PRESENTVSYNC)
			puts("renderer: uses vsync");
		if (renderer_info.flags & SDL_RENDERER_TARGETTEXTURE)
			puts("renderer: supports rendering to texture");
		printf("renderer: available texture formats:");
		unsigned const n_texture_formats = min(16u, (unsigned)renderer_info.num_texture_formats);
		for (unsigned i = 0; i < n_texture_formats; ++i)
			printf(" %s", SDL_GetPixelFormatName(renderer_info.texture_formats[i]));
		putchar('\n');
	}

	fail_if(!(screen_tex =
				SDL_CreateTexture(
					renderer,
					// SDL takes endianess into account, so this becomes GL_RGBA8
					// internally on little-endian systems
					SDL_PIXELFORMAT_ARGB8888,
					SDL_TEXTUREACCESS_STREAMING,
					280, 240)),
			"failed to create texture for screen: %s", SDL_GetError());
	
	SDL_Surface* dbgfontsurf;
	fail_if(!(dbgfontsurf = IMG_ReadXPMFromArray(dbgfont_xpm)),"failed to load debug font: %s", SDL_GetError());

	dbg_font = SDL_CreateTextureFromSurface(renderer,dbgfontsurf);
	SDL_FreeSurface(dbgfontsurf);

	static Uint32 render_buffers[2][240*256];
	back_buffer  = render_buffers[0];
	front_buffer = render_buffers[1];
	
	// Audio

	SDL_AudioSpec want;
	SDL_zero(want);
	want.freq     = sample_rate;
	want.format   = AUDIO_S16SYS;
	want.channels = 1;
	want.samples  = sdl_audio_buffer_size;
	want.callback = audio_callback;

	fail_if(!(audio_device_id = SDL_OpenAudioDevice(0, 0, &want, 0, 0)),
			"failed to initialize audio: %s\n", SDL_GetError());

	// Input

	// We use SDL_GetKey/MouseState() instead
	SDL_EventState(SDL_KEYDOWN        , SDL_IGNORE);
	SDL_EventState(SDL_KEYUP          , SDL_IGNORE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_IGNORE);
	SDL_EventState(SDL_MOUSEBUTTONUP  , SDL_IGNORE);
	SDL_EventState(SDL_KEYUP          , SDL_IGNORE);
	SDL_EventState(SDL_MOUSEMOTION    , SDL_IGNORE);

	// Ignore window events for now
	//SDL_EventState(SDL_WINDOWEVENT, SDL_IGNORE);


	int oldksz = keys_size;
	keys = SDL_GetKeyboardState(&keys_size);
	if (keys_size != oldksz) keys_lf = (Uint8*) realloc(keys_lf, keys_size * sizeof(Uint8));

	// SDL thread synchronization

	fail_if(!(event_lock = SDL_CreateMutex()),
			"failed to create event mutex: %s", SDL_GetError());

	fail_if(!(frame_lock = SDL_CreateMutex()),
			"failed to create frame mutex: %s", SDL_GetError());
	fail_if(!(frame_available_cond = SDL_CreateCond()),
			"failed to create frame condition variable: %s", SDL_GetError());
}

void sdldbg_scroll(void) {
	memcpy(debug_contents, debug_contents + 128, 128*59);
	memcpy(debug_colors, debug_colors + 128, 128*59);
}

int sdldbg_puts(const char* s) {

	const unsigned char* curchar = (const unsigned char*) s;

	while (*curchar) {

		if ((*curchar >= 32) && (*curchar < 128)) {
			debug_contents[debug_cur_y * 128 + debug_cur_x] = *curchar;
			debug_colors[debug_cur_y * 128 + debug_cur_x] = debug_cur_color;
			debug_cur_x++;
		}
		if ((*curchar == 10) || (*curchar == 13)) {
			debug_cur_y++;
			debug_cur_x = 0;
		}
		if ((*curchar >= 240)) {
			debug_cur_color = (*curchar) - 240;
		}

		curchar++;
		if (debug_cur_x >= 128) {debug_cur_x = 0; debug_cur_y++;}
		if (debug_cur_y >= 60) {sdldbg_scroll(); debug_cur_y = 59;}
	}
	return 0;
}

int mvsdldbg_puts(const char* s,const char x, const char y) {
	debug_cur_x = x; debug_cur_y = y;
	return sdldbg_puts(s);
}

int sdldbg_vprintf(const char* format, va_list ap) {

	int len = 64;

	char* o_text = (char*)malloc(len);

	int n = vsnprintf(o_text, len, format, ap);
	if ((n+1) > len) {
		o_text = (char*)realloc(o_text, n+1);
		n = vsnprintf(o_text, n+1, format, ap);
	}


	//puts(o_text);

	int r = sdldbg_puts(o_text);
	free(o_text);
	return r;
}

int sdldbg_printf(const char* format, ...) {

	va_list ap;

	va_start(ap, format);
	int r = sdldbg_vprintf(format, ap);
	va_end(ap);

	return r;
}

int mvsdldbg_printf(int x, int y, const char* format, ...) {

	va_list ap;

	va_start(ap, format);
	debug_cur_x = x; debug_cur_y = y;
	int r = sdldbg_vprintf(format, ap) ;
	va_end(ap);
	return r;
}

int sdl_text_prompt(const char* prompt, char* value, int value_sz) {

	int loop = 1;
	
	mvsdldbg_printf(0, 58, "%-120s");
	mvsdldbg_printf(0, 59, " > ");
	
	cur_textinput = value;
	cur_textinput_sz = value_sz;

	SDL_StartTextInput();
	
	SDL_StopTextInput();
	return 1;
}

void deinit_sdl() {
	SDL_DestroyRenderer(renderer); // Also destroys the texture
	SDL_DestroyWindow(screen);

	SDL_DestroyMutex(event_lock);

	SDL_DestroyMutex(frame_lock);
	SDL_DestroyCond(frame_available_cond);

	SDL_CloseAudioDevice(audio_device_id); // Prolly not needed, but play it safe
	SDL_Quit();
}
