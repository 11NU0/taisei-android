#include "setup.h"

#ifdef __ANDROID__

#include "platform_paths/platform_paths.h"
#include "public.h"
#include "log.h"
#include "memory/memory.h"
#include "util/env.h"
#include "util/strbuf.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#define ANDROID_ASSET_MANIFEST "taisei/files.list"
#define ANDROID_ASSET_VERSION "taisei/version.txt"
#define ANDROID_RES_SUBDIR "data"

static AAssetManager *android_get_asset_manager(void) {
	JNIEnv *env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	jobject activity = (jobject)SDL_GetAndroidActivity();

	if(env == NULL || activity == NULL) {
		log_error("Could not acquire JNI environment or activity");
		return NULL;
	}

	jclass cls = (*env)->GetObjectClass(env, activity);

	if(cls == NULL) {
		log_error("GetObjectClass failed");
		return NULL;
	}

	jmethodID mid = (*env)->GetMethodID(env, cls, "getAssets", "()Landroid/content/res/AssetManager;");

	if(mid == NULL) {
		(*env)->DeleteLocalRef(env, cls);
		log_error("Could not find AssetManager getter");
		return NULL;
	}

	jobject assets = (*env)->CallObjectMethod(env, activity, mid);
	AAssetManager *mgr = assets ? AAssetManager_fromJava(env, assets) : NULL;

	if(assets) {
		(*env)->DeleteLocalRef(env, assets);
	}

	(*env)->DeleteLocalRef(env, cls);
	return mgr;
}

static void *android_read_asset(AAssetManager *mgr, const char *path, size_t *out_size) {
	AAsset *asset = AAssetManager_open(mgr, path, AASSET_MODE_STREAMING);

	if(asset == NULL) {
		return NULL;
	}

	size_t size = AAsset_getLength(asset);
	char *buf = mem_alloc(size + 1);
	size_t offset = 0;

	while(offset < size) {
		int n = AAsset_read(asset, buf + offset, size - offset);

		if(n <= 0) {
			break;
		}

		offset += n;
	}

	AAsset_close(asset);
	buf[offset] = 0;

	if(out_size) {
		*out_size = offset;
	}

	return buf;
}

static bool android_extract_asset(AAssetManager *mgr, const char *asset_path, const char *dest_path) {
	size_t size;
	void *data = android_read_asset(mgr, asset_path, &size);

	if(data == NULL) {
		log_error("Missing APK asset: %s", asset_path);
		return false;
	}

	char *dir = SDL_strdup(dest_path);
	char *slash = strrchr(dir, '/');

	if(slash) {
		*slash = 0;

		if(!SDL_CreateDirectory(dir)) {
			log_warn("SDL_CreateDirectory('%s'): %s", dir, SDL_GetError());
		}
	}

	SDL_free(dir);

	SDL_IOStream *out = SDL_IOFromFile(dest_path, "wb");

	if(out == NULL) {
		log_error("Can not write '%s': %s", dest_path, SDL_GetError());
		mem_free(data);
		return false;
	}

	bool ok = SDL_WriteIO(out, data, size) == size && SDL_CloseIO(out);

	if(!ok) {
		log_error("Failed to extract '%s'", dest_path);
	}

	mem_free(data);
	return ok;
}

static char *android_read_text_file(const char *path) {
	SDL_IOStream *io = SDL_IOFromFile(path, "rb");

	if(io == NULL) {
		return NULL;
	}

	size_t size = 0;
	char *data = SDL_LoadFile_IO(io, &size, true);

	if(data && size) {
		data[size] = 0;
	}

	return data;
}

static void android_extract_resources(const char *res_path) {
	AAssetManager *mgr = android_get_asset_manager();

	if(mgr == NULL) {
		log_warn("No AssetManager; using resources already present in %s", res_path);
		return;
	}

	char *manifest = android_read_asset(mgr, ANDROID_ASSET_MANIFEST, NULL);

	if(manifest == NULL) {
		log_warn("APK has no asset manifest; using resources already present in %s", res_path);
		return;
	}

	char *version = android_read_asset(mgr, ANDROID_ASSET_VERSION, NULL);

	if(version == NULL) {
		version = SDL_strdup("unknown");
	}

	char *stamp_path = NULL;
	SDL_asprintf(&stamp_path, "%s/.assets-version", res_path);

	bool up_to_date = false;
	char *stamp = stamp_path ? android_read_text_file(stamp_path) : NULL;

	if(stamp && !strcmp(stamp, version)) {
		up_to_date = true;
	}

	SDL_free(stamp);

	if(up_to_date) {
		log_info("Bundled resources are up to date (%s)", version);
		SDL_free(version);
		SDL_free(stamp_path);
		mem_free(manifest);
		return;
	}

	log_info("Extracting bundled resources to %s", res_path);
	SDL_CreateDirectory(res_path);

	size_t extracted = 0;
	char *saveptr = NULL;

	for(char *line = SDL_strtok_r(manifest, "\n", &saveptr); line; line = SDL_strtok_r(NULL, "\n", &saveptr)) {
		while(*line == ' ' || *line == '\t' || *line == '\r') {
			++line;
		}

		size_t len = strlen(line);

		while(len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
			line[--len] = 0;
		}

		if(len == 0 || *line == '#') {
			continue;
		}

		char *dest = NULL;
		SDL_asprintf(&dest, "%s/%s", res_path, line);

		if(dest && android_extract_asset(mgr, line, dest)) {
			++extracted;
		}

		SDL_free(dest);
	}

	log_info("Extracted %u resource files", (uint)extracted);

	if(stamp_path) {
		SDL_IOStream *io = SDL_IOFromFile(stamp_path, "wb");

		if(io) {
			SDL_WriteIO(io, version, strlen(version));
			SDL_CloseIO(io);
		}
	}

	SDL_free(version);
	SDL_free(stamp_path);
	mem_free(manifest);
}

void vfs_setup(CallChain next) {
	const char *internal = SDL_GetAndroidInternalStoragePath();

	if(internal) {
		char *res_path = NULL;
		SDL_asprintf(&res_path, "%s/%s", internal, ANDROID_RES_SUBDIR);

		if(res_path) {
			android_extract_resources(res_path);
			env_set("TAISEI_RES_PATH", res_path, false);
			log_info("Android resource path: %s", res_path);
			SDL_free(res_path);
		}
	} else {
		log_error("SDL_GetAndroidInternalStoragePath failed: %s", SDL_GetError());
	}

	vfs_setup_generic(next);
}

#endif
