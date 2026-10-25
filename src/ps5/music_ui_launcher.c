#include <stdio.h>

#ifndef PS5_MUSIC_UI_URL
#error "Define PS5_MUSIC_UI_URL with the library server address reachable by the PS5."
#endif

int sceUserServiceInitialize(void* parameter);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char* uri, void* parameter);

int main(void) {
    const int initialize_result = sceUserServiceInitialize(NULL);
    if (initialize_result != 0) {
        printf("sceUserServiceInitialize failed: 0x%x\n", initialize_result);
        return 1;
    }

    const int launch_result = sceSystemServiceLaunchWebBrowser(PS5_MUSIC_UI_URL, NULL);
    if (launch_result != 0) {
        printf("Could not open the PS5-SACD Music Room at %s: 0x%x\n",
               PS5_MUSIC_UI_URL,
               launch_result);
    }

    const int terminate_result = sceUserServiceTerminate();
    if (terminate_result != 0) {
        printf("sceUserServiceTerminate failed: 0x%x\n", terminate_result);
    }
    return launch_result == 0 ? 0 : 1;
}
