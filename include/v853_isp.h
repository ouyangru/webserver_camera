#ifndef WEBSERVER_V853_ISP_H
#define WEBSERVER_V853_ISP_H
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#if ENABLE_CEDAR
#include <AWIspApi.h>
#endif
class V853Isp {
    void* library = NULL;
#if ENABLE_CEDAR
    AWIspApi* api = NULL;
    void (*destroy)(AWIspApi*) = NULL;
    int id = -1;
    bool initialized = false, running = false;
#endif
public:
    bool start(int video_id) {
#if ENABLE_CEDAR
        const char* disable = getenv("DISABLE_ISP");
        if (disable && atoi(disable)) return true;
        library = dlopen("libAWIspApi.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) { fprintf(stderr, "[ISP] %s\n", dlerror()); return false; }
        auto create = reinterpret_cast<AWIspApi*(*)()>(dlsym(library, "CreateAWIspApi"));
        destroy = reinterpret_cast<void(*)(AWIspApi*)>(dlsym(library, "DestroyAWIspApi"));
        if (!create || !destroy || !(api = create())) return false;
        if (api->ispApiInit() < 0) return false;
        initialized = true;
        id = api->ispGetIspId(video_id);
        if (id < 0 || api->ispStart(id) < 0) return false;
        running = true;
        printf("[ISP] started isp%d for video%d: automatic exposure/white balance\n", id, video_id);
#else
        (void)video_id;
#endif
        return true;
    }
    ~V853Isp() {
#if ENABLE_CEDAR
        if (running) api->ispStop(id);
        if (initialized) api->ispApiUnInit();
        if (api && destroy) destroy(api);
#endif
        if (library) dlclose(library);
    }
};
#endif
