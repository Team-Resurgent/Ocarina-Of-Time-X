// NoGui.cpp -- libultraship with no GUI (LUS_NO_IMGUI, the Xbox build): Gui, GameOverlay and the
// console window's entry points, doing nothing that needs ImGui. The game draws straight into the
// screen (Interpreter::StartFrame), notifications and console messages go to the log, and the
// windows a game registers are dropped (GetGuiWindow finds none).
#ifdef LUS_NO_IMGUI

#include "ship/window/gui/Gui.h"
#include "ship/window/gui/GameOverlay.h"
#include "ship/window/gui/ConsoleWindow.h"
#include "ship/window/gui/GuiMenuBar.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"

#include <cstdarg>
#include <spdlog/spdlog.h>

namespace Ship {

static std::string Format(const char* fmt, va_list ap) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

Gui::Gui() : mNeedsConsoleVariableSave(false), mGameOverlay(std::make_shared<GameOverlay>()) {
}

Gui::Gui(std::vector<std::shared_ptr<GuiWindow>> guiWindows) : Gui() {
}

Gui::~Gui() {
}

void Gui::Init() {
}

void Gui::StartDraw() {
}

void Gui::EndDraw() {
    CheckSaveCvars();
}

void Gui::CheckSaveCvars() {
    if (mNeedsConsoleVariableSave) {
        Context::GetRawInstance()->GetConsoleVariables()->Save();
        mNeedsConsoleVariableSave = false;
    }
}

void Gui::SaveConsoleVariablesNextFrame() {
    mNeedsConsoleVariableSave = true;
}

bool Gui::SupportsViewports() {
    return false;
}

ImGuiID Gui::GetMainGameWindowID() {
    return 0;
}

void Gui::AddGuiWindow(std::shared_ptr<GuiWindow> guiWindow) {
}

std::shared_ptr<GuiWindow> Gui::GetGuiWindow(const std::string& name) {
    return nullptr;
}

void Gui::RemoveGuiWindow(std::shared_ptr<GuiWindow> guiWindow) {
}

void Gui::RemoveGuiWindow(const std::string& name) {
}

void Gui::RemoveAllGuiWindows() {
}

std::shared_ptr<GameOverlay> Gui::GetGameOverlay() {
    return mGameOverlay;
}

void Gui::SetMenuBar(std::shared_ptr<GuiMenuBar> menuBar) {
}

std::shared_ptr<GuiMenuBar> Gui::GetMenuBar() {
    return nullptr;
}

void Gui::SetMenu(std::shared_ptr<GuiWindow> menu) {
}

std::shared_ptr<GuiWindow> Gui::GetMenu() {
    return nullptr;
}

bool Gui::GetMenuOrMenubarVisible() {
    return false;
}

bool Gui::IsMouseOverAnyGuiItem() {
    return false;
}

bool Gui::IsMouseOverActivePopup() {
    return false;
}

bool Gui::GamepadNavigationEnabled() {
    return false;
}

void Gui::BlockGamepadNavigation() {
}

void Gui::UnblockGamepadNavigation() {
}

void Gui::RefreshImGuiGamepads() {
}

void Gui::UpdateGamepadNavigation() {
}

void Gui::ShutDownImGui(Ship::Window* window) {
}

void Gui::StartFrame() {
}

void Gui::EndFrame() {
}

void Gui::DrawFloatingWindows() {
}

void Gui::DrawMenu() {
}

void Gui::DrawGame() {
}

void Gui::CalculateGameViewport() {
}

void Gui::ImGuiBackendNewFrame() {
}

void Gui::ImGuiWMNewFrame() {
}

void Gui::ImGuiWMInit() {
}

void Gui::ImGuiWMShutdown() {
}

void Gui::ImGuiBackendInit() {
}

void Gui::ImGuiBackendShutdown() {
}

void Gui::ImGuiRenderDrawData(ImDrawData* data) {
}

void Gui::HandleMouseCapture() {
}

GameOverlay::GameOverlay() {
}

GameOverlay::~GameOverlay() {
}

void GameOverlay::Init() {
}

void GameOverlay::LoadFont(const std::string& name, float fontSize, const ResourceIdentifier& identifier) {
}

void GameOverlay::LoadFont(const std::string& name, float fontSize, const std::string& path) {
}

void GameOverlay::SetCurrentFont(const std::string& name) {
}

void GameOverlay::Draw() {
}

void GameOverlay::DrawSettings() {
}

float GameOverlay::GetStringWidth(const char* text) {
    return 0.0f;
}

ImVec2 GameOverlay::CalculateTextSize(const char* text, const char* textEnd, bool shortenText, float wrapWidth) {
    return ImVec2();
}

void GameOverlay::TextDraw(float x, float y, bool shadow, ImVec4 color, const char* fmt, ...) {
}

void GameOverlay::TextDrawNotification(float duration, bool shadow, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SPDLOG_INFO("Overlay: {}", Format(fmt, ap));
    va_end(ap);
}

void GameOverlay::ClearNotifications() {
}

ImGuiID GameOverlay::GetID() {
    return 0;
}

float GameOverlay::GetScreenWidth() {
    return 640.0f;
}

float GameOverlay::GetScreenHeight() {
    return 480.0f;
}

void GameOverlay::CleanupNotifications() {
}

// the console window is never made: these are called through a null window pointer by code that
// expects one, and touch nothing of it
void ConsoleWindow::Dispatch(const std::string& line) {
    SPDLOG_INFO("Console (not available): {}", line);
}

void ConsoleWindow::SendInfoMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SPDLOG_INFO("Console: {}", Format(fmt, ap));
    va_end(ap);
}

void ConsoleWindow::SendErrorMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SPDLOG_ERROR("Console: {}", Format(fmt, ap));
    va_end(ap);
}

void ConsoleWindow::SendInfoMessage(const std::string& str) {
    SPDLOG_INFO("Console: {}", str);
}

void ConsoleWindow::SendErrorMessage(const std::string& str) {
    SPDLOG_ERROR("Console: {}", str);
}

} // namespace Ship

#endif
