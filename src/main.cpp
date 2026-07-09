// Entry point. Kept intentionally tiny: all work lives in App.
//
// GUI subsystem (matches the original sampcmd.exe, so no console window pops up
// during a normal Steam/desktop launch). --console opts into a console at runtime.
#include "windows_prelude.hpp"
#include "app.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    App app;
    return app.run();
}
