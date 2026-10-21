// Dino Crisis ME Studio — просмотр и выгрузка содержимого игры, конвертация файлов mods.
// Окно: SDL2 + OpenGL 3.3 + Dear ImGui. Проверка без человека: --shot <png> [--frames N] [--select ST100] [--tab 1] [--item 0]
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>

#include "gfx.h"
#include "i18n.h"
#include "ui.h"

using namespace dc;

namespace dc {
bool write_png(const fs::path& p, int w, int h, const uint32_t* rgba);
}

static void apply_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.ChildRounding = 4;
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.86f, 0.42f, 0.16f, 1.0f), accent_h(0.95f, 0.52f, 0.22f, 1.0f), accent_d(0.62f, 0.28f, 0.10f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.11f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.13f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.14f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.14f, 0.15f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.21f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.27f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.29f, 0.31f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.17f, 0.19f, 1);
    c[ImGuiCol_Button] = accent_d;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = accent_h;
    c[ImGuiCol_Header] = ImVec4(0.55f, 0.26f, 0.10f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.36f, 0.14f, 0.75f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.18f, 0.19f, 0.21f, 1);
    c[ImGuiCol_TabHovered] = accent;
    c[ImGuiCol_TabSelected] = accent_d;
    c[ImGuiCol_CheckMark] = accent_h;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_h;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_ResizeGrip] = accent_d;
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.86f, 0.42f, 0.16f, 0.35f);
    s.ScaleAllSizes(scale);
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--names")) {   // сборка пакета игры (rebuild.ps1): mods\names.txt без окна
            App app;
            const int r = app.names_only(argc, argv);
            CoUninitialize();
            return r;
        }
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) return 1;
    bool shot = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--shot")) shot = true;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    float ddpi = 96;
    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) != 0 || ddpi <= 0) ddpi = 96;
    float scale = std::max(1.0f, ddpi / 96.0f);
    SDL_Rect usable{0, 0, 1600, 900};
    SDL_GetDisplayUsableBounds(0, &usable);
    int ww = std::min((int)(1440 * scale), usable.w - 40), wh = std::min((int)(900 * scale), usable.h - 60);
    if (shot) { ww = 1600; wh = 900; scale = 1; }
    SDL_Window* win = SDL_CreateWindow("Dino Crisis ME Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (shot ? SDL_WINDOW_HIDDEN : 0));
    if (!win) return 1;
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    gl::load();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_style(scale);
    const char* fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", nullptr};
    bool font = false;
    for (int i = 0; fonts[i] && !font; i++)
        if (fs::exists(fonts[i])) font = io.Fonts->AddFontFromFileTTF(fonts[i], 17.0f * scale) != nullptr;
    if (!font) io.Fonts->AddFontDefault();
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    app.init(win, argc, argv);
    if (!shot) SDL_ShowWindow(win);
    // снимок: рисуем в свою текстуру (окно скрыто — его буфер не годится)
    unsigned sfbo = 0, stex = 0;
    if (shot) {
        gl::GenFramebuffers(1, &sfbo);
        gl::BindFramebuffer(GL_FRAMEBUFFER, sfbo);
        glGenTextures(1, &stex);
        glBindTexture(GL_TEXTURE_2D, stex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ww, wh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stex, 0);
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    while (!app.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) app.quit = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(win)) app.quit = true;
            if (e.type == SDL_DROPFILE) {
                app.on_drop(e.drop.file);
                SDL_free(e.drop.file);
            }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        if (shot) io.DisplaySize = ImVec2((float)ww, (float)wh);
        app.input_probe();   // 14.10.2026: checks that move the mouse themselves (after the backend's own mouse position)
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        int dw = ww, dh = wh;
        if (!shot) SDL_GL_GetDrawableSize(win, &dw, &dh);
        gl::BindFramebuffer(GL_FRAMEBUFFER, sfbo);
        glViewport(0, 0, dw, dh);
        glClearColor(0.09f, 0.10f, 0.11f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && app.frame_no >= app.shot_after && !app.busy() && app.idle_frames >= 3) {
            std::vector<uint32_t> px((size_t)dw * dh), fl((size_t)dw * dh);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < dh; y++) memcpy(&fl[(size_t)y * dw], &px[(size_t)(dh - 1 - y) * dw], (size_t)dw * 4);
            for (auto& p : fl) p |= 0xFF000000u;
            write_png(u8path(app.shot_path), dw, dh, fl.data());
            app.quit = true;
        }
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_GL_SwapWindow(win);
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    CoUninitialize();
    return 0;
}
