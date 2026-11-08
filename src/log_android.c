#include "log_android.h"

#ifdef __ANDROID__

#include "log.h"
#include "memory/memory.h"

#include <SDL3/SDL.h>
#include <android/log.h>

typedef struct LogcatState {
	char buf[2048];
	size_t len;
} LogcatState;

static void logcat_emit(LogcatState *st) {
	if(st->len == 0) {
		return;
	}

	st->buf[st->len] = 0;
	__android_log_write(ANDROID_LOG_INFO, "taisei", st->buf);
	st->len = 0;
}

static size_t SDLCALL logcat_write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status) {
	LogcatState *st = userdata;
	const char *p = ptr;

	for(size_t i = 0; i < size; ++i) {
		char c = p[i];

		if(c == '\r') {
			continue;
		}

		if(c == '\n' || st->len == sizeof(st->buf) - 1) {
			logcat_emit(st);
		}

		if(c != '\n') {
			st->buf[st->len++] = c;
		}
	}

	return size;
}

static bool SDLCALL logcat_flush(void *userdata, SDL_IOStatus *status) {
	logcat_emit(userdata);
	return true;
}

static bool SDLCALL logcat_close(void *userdata) {
	logcat_emit(userdata);
	mem_free(userdata);
	return true;
}

void log_android_init(void) {
	LogcatState *st = ALLOC(LogcatState);

	SDL_IOStreamInterface iface;
	SDL_INIT_INTERFACE(&iface);
	iface.write = logcat_write;
	iface.flush = logcat_flush;
	iface.close = logcat_close;

	SDL_IOStream *io = SDL_OpenIO(&iface, st);

	if(io == NULL) {
		mem_free(st);
		__android_log_write(ANDROID_LOG_ERROR, "taisei", "Failed to create logcat stream");
		return;
	}

	log_add_output(LOG_ALL | LOG_UNFILTERED_BIT, io, log_formatter_console);
}

#endif
