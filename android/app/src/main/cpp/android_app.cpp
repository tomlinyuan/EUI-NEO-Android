#include "app/dsl_app.h"

#include "components/components.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" const char* eui_android_text_font_path(void);
extern "C" const char* eui_android_icon_font_path(void);
extern "C" const char* eui_android_files_dir(void);

namespace app {

namespace {

// ── Layout constants ──────────────────────────────────────────────
constexpr float kSidebarWidth      = 92.0f;
constexpr float kSidebarBrandH     = 70.0f;
constexpr float kSidebarItemH      = 64.0f;
constexpr float kSidebarItemGap    = 10.0f;
constexpr float kSidebarItemTop    = 14.0f;
constexpr float kContentPaddingX   = 14.0f;

// ── Page state ────────────────────────────────────────────────────
int   g_selectedPage = 0;
float g_pageScroll[6] = {0, 0, 0, 0, 0, 0};

// ── Controls state ────────────────────────────────────────────────
bool          g_switchOn       = true;
bool          g_switchDark     = false;
bool          g_checkA         = true;
bool          g_checkB         = false;
bool          g_checkC         = false;
int           g_radioSelect    = 2;   // 0=light, 1=dark, 2=auto (default follows system)
bool          g_systemDarkMode = true; // set from Java; default to dark before first config read
float         g_slider1        = 0.5f;
float         g_slider2        = 0.25f;
float         g_progress       = 0.65f;
int           g_segment        = 0;
int           g_tab            = 0;
int           g_stepper        = 10;
int           g_dropdown       = 0;
bool          g_dropdownOpen   = false;
std::string   g_input          = "";
std::string   g_inputCn        = "";
std::string   g_toastMsg       = "欢迎使用 EUI-NEO!";
bool          g_toastVisible   = true;
bool          g_dialogOpen     = false;
bool          g_datePickerOpen = false;
bool          g_timePickerOpen = false;
bool          g_colorPickerOpen= false;
int           g_year           = 2026;
int           g_month          = 5;
int           g_day            = 24;
int           g_hour           = 14;
int           g_minute         = 30;
core::Color   g_pickedColor    = {0.30f, 0.60f, 1.00f, 1.0f};

std::vector<float> g_lineSeries = {12.0f, 28.0f, 18.0f, 36.0f, 42.0f, 30.0f, 48.0f};
std::vector<float> g_barSeries  = {18.0f, 26.0f, 14.0f, 32.0f, 22.0f};
std::vector<float> g_pieSeries  = {30.0f, 22.0f, 18.0f, 15.0f, 15.0f};

// Frame tick driven by an onTimer in the chart/clock pages. Incrementing it
// only matters as a signal to needsCompose — the actual animation value is
// derived from this counter so chart bars/line ripple over time.
int g_tick = 0;

} // namespace (close anonymous so the extern "C" hook can refer to the
  // variables above without the linker mangling away its C symbol)

// Called from JNI when Android's uiMode changes (system theme switched).
// Defined here so the value is visible to effectiveDark() in the next
// recompose.
extern "C" void eui_android_set_system_dark_mode(int dark) {
    g_systemDarkMode = dark != 0;
}

namespace {
// ── Theme ──────────────────────────────────────────────────────────
// g_radioSelect doubles as the theme mode:
//   0 = 浅色 (force light)
//   1 = 深色 (force dark)
//   2 = 自动 (follow system uiMode, reported by Java via nativeSetDarkMode)
bool effectiveDark() {
    if (g_radioSelect == 0) return false;
    if (g_radioSelect == 1) return true;
    return g_systemDarkMode;
}

components::theme::ThemeColorTokens themeColors() {
    auto tokens = effectiveDark()
        ? components::theme::DarkThemeColors()
        : components::theme::LightThemeColors();
    tokens.primary = g_pickedColor;
    return tokens;
}

core::Color appBg()     { return themeColors().background; }
core::Color sidebarBg() {
    const auto t = themeColors();
    return t.dark
        ? core::mixColor(t.background, {0.0f, 0.0f, 0.0f, 1.0f}, 0.30f)
        : core::mixColor(t.background, {0.0f, 0.0f, 0.0f, 1.0f}, 0.06f);
}
core::Color surface()  { return themeColors().surface; }
core::Color surface2() {
    const auto t = themeColors();
    return core::mixColor(t.surface, t.background, 0.40f);
}
core::Color accent()   { return g_pickedColor; }
core::Color textPri()  { return themeColors().text; }
core::Color textSec()  { return components::theme::withAlpha(themeColors().text, 0.70f); }
core::Color textDim()  { return components::theme::withAlpha(themeColors().text, 0.55f); }
core::Color borderC()  { return themeColors().border; }
core::Color dangerC()  { return {0.95f, 0.30f, 0.35f, 1.0f}; }

core::Color btnHover(core::Color c) { return core::mixColor(c, {1.0f, 1.0f, 1.0f, c.a}, 0.16f); }
core::Color btnPress(core::Color c) { return core::mixColor(c, {0.04f, 0.05f, 0.08f, c.a}, 0.30f); }

void showToast(const std::string& msg) {
    g_toastMsg = msg;
    g_toastVisible = true;
}

// ── Settings persistence ──────────────────────────────────────────
// Saved to <files-dir>/settings.txt as key=value lines. Loaded once on the
// first compose. Saved on every state change so quitting the app at any
// point preserves the latest user choices.

std::string settingsPath() {
    const char* dir = eui_android_files_dir();
    return dir ? std::string(dir) + "/settings.txt" : std::string();
}

void saveSettings() {
    // The settings file is ~500 bytes — writing it goes through the kernel
    // page cache so even slider drags at 60 Hz don't actually hit storage on
    // every frame. Saving on every change keeps state durable without losing
    // late updates to a throttle window.
    const std::string path = settingsPath();
    if (path.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f.good()) return;
    f << "selectedPage="   << g_selectedPage     << "\n";
    f << "switchOn="       << (g_switchOn ? 1 : 0)   << "\n";
    f << "switchDark="     << (g_switchDark ? 1 : 0) << "\n";
    f << "checkA="         << (g_checkA ? 1 : 0)     << "\n";
    f << "checkB="         << (g_checkB ? 1 : 0)     << "\n";
    f << "checkC="         << (g_checkC ? 1 : 0)     << "\n";
    f << "radioSelect="    << g_radioSelect      << "\n";
    f << "slider1="        << g_slider1          << "\n";
    f << "slider2="        << g_slider2          << "\n";
    f << "segment="        << g_segment          << "\n";
    f << "tab="            << g_tab              << "\n";
    f << "stepper="        << g_stepper          << "\n";
    f << "dropdown="       << g_dropdown         << "\n";
    f << "input="          << g_input            << "\n";
    f << "inputCn="        << g_inputCn          << "\n";
    f << "year="           << g_year             << "\n";
    f << "month="          << g_month            << "\n";
    f << "day="            << g_day              << "\n";
    f << "hour="           << g_hour             << "\n";
    f << "minute="         << g_minute           << "\n";
    f << "color="          << g_pickedColor.r << "," << g_pickedColor.g << ","
                           << g_pickedColor.b << "," << g_pickedColor.a << "\n";
}

void loadSettings() {
    const std::string path = settingsPath();
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.good()) return;

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }

    auto asInt = [&](const std::string& k, int def) {
        auto it = kv.find(k); if (it == kv.end()) return def;
        const char* s = it->second.c_str();
        char* end = nullptr;
        long v = std::strtol(s, &end, 10);
        return (end == s) ? def : static_cast<int>(v);
    };
    auto asFloat = [&](const std::string& k, float def) {
        auto it = kv.find(k); if (it == kv.end()) return def;
        const char* s = it->second.c_str();
        char* end = nullptr;
        float v = std::strtof(s, &end);
        return (end == s) ? def : v;
    };
    auto asBool = [&](const std::string& k, bool def) {
        return asInt(k, def ? 1 : 0) != 0;
    };
    auto asStr = [&](const std::string& k, const std::string& def) {
        auto it = kv.find(k); return it == kv.end() ? def : it->second;
    };

    g_selectedPage = std::clamp(asInt("selectedPage", 0), 0, 5);
    g_switchOn     = asBool("switchOn",   g_switchOn);
    g_switchDark   = asBool("switchDark", g_switchDark);
    g_checkA       = asBool("checkA",     g_checkA);
    g_checkB       = asBool("checkB",     g_checkB);
    g_checkC       = asBool("checkC",     g_checkC);
    g_radioSelect  = asInt ("radioSelect",g_radioSelect);
    g_slider1      = asFloat("slider1",   g_slider1);
    g_slider2      = asFloat("slider2",   g_slider2);
    g_segment      = asInt ("segment",    g_segment);
    g_tab          = asInt ("tab",        g_tab);
    g_stepper      = asInt ("stepper",    g_stepper);
    g_dropdown     = asInt ("dropdown",   g_dropdown);
    g_input        = asStr ("input",      g_input);
    g_inputCn      = asStr ("inputCn",    g_inputCn);
    g_year         = asInt ("year",       g_year);
    g_month        = asInt ("month",      g_month);
    g_day          = asInt ("day",        g_day);
    g_hour         = asInt ("hour",       g_hour);
    g_minute       = asInt ("minute",     g_minute);

    auto it = kv.find("color");
    if (it != kv.end()) {
        std::istringstream ss(it->second);
        std::string part;
        float c[4] = {g_pickedColor.r, g_pickedColor.g, g_pickedColor.b, g_pickedColor.a};
        for (int i = 0; i < 4 && std::getline(ss, part, ','); ++i) {
            const char* s = part.c_str();
            char* end = nullptr;
            float v = std::strtof(s, &end);
            if (end != s) c[i] = v;
        }
        g_pickedColor = {c[0], c[1], c[2], c[3]};
    }
}

// ── Card section helper — overlays content column on top of panel ──
// This is the correct way to put controls "inside" a card frame: a stack
// places the panel rect and the content column on the same z-layer, so
// they share bounds instead of stacking vertically.
//
// zIndex lets dropdown sections sit on top of subsequent siblings so an
// expanded popup isn't drawn over by the next card.
template <typename Body>
void cardSection(core::dsl::Ui& ui, const std::string& id, float width, float height,
                 Body&& body, int zIndex = 0) {
    const core::Transition sizeTransition = core::Transition::make(0.22f, core::Ease::OutCubic);
    ui.stack(id + ".wrap")
        .size(width, height)
        .zIndex(zIndex)
        .transition(sizeTransition)
        .animate(core::AnimProperty::Frame)
        .content([&] {
            components::panel(ui, id + ".card")
                .size(width, height)
                .radius(14.0f)
                .gradient(surface(), surface2())
                .border(1.0f, borderC())
                .shadow(14.0f, 0.0f, 5.0f, {0.0f, 0.0f, 0.0f, 0.22f})
                .transition(sizeTransition)
                .animate(core::AnimProperty::Frame)
                .build();

            ui.column(id + ".body")
                .size(width, height)
                .padding(14.0f, 14.0f, 14.0f, 14.0f)
                .gap(10.0f)
                .justifyContent(core::Align::CENTER)
                .alignItems(core::Align::CENTER)
                .content([&] { body(); });
        });
}

void sectionTitle(core::dsl::Ui& ui, const std::string& id, const std::string& title,
                  const std::string& subtitle, float width) {
    ui.column(id + ".hdr")
        .width(width)
        .height(core::SizeValue::wrapContent())
        .margin(0.0f, 18.0f, 0.0f, 8.0f)
        .gap(2.0f)
        .content([&] {
            components::text(ui, id + ".hdr.title")
                .size(width, 24.0f)
                .text(title)
                .fontSize(18.0f)
                .fontWeight(700)
                .color(accent())
                .build();
            if (!subtitle.empty()) {
                components::text(ui, id + ".hdr.sub")
                    .size(width, 18.0f)
                    .text(subtitle)
                    .fontSize(12.0f)
                    .color(textSec())
                    .build();
            }
        });
}

// ── Sidebar ────────────────────────────────────────────────────────
int navOrderForPage(int page) { return std::clamp(page, 0, 5); }

void sidebarItem(core::dsl::Ui& ui, const std::string& id, const std::string& label,
                 unsigned int icon, int page) {
    const bool active = g_selectedPage == page;
    const core::Color activeC = accent();
    const core::Color base    = active ? activeC : sidebarBg();
    const core::Color hover   = active ? btnHover(activeC) : surface();
    const core::Color press   = active ? btnPress(activeC) : surface2();
    const core::Color iconC   = active ? core::Color{1.0f, 1.0f, 1.0f, 1.0f} : textSec();
    const core::Color labelC  = active ? core::Color{1.0f, 1.0f, 1.0f, 1.0f} : textDim();

    ui.stack(id)
        .size(kSidebarWidth - 12.0f, kSidebarItemH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(kSidebarWidth - 12.0f, kSidebarItemH)
                .states(base, hover, press)
                .radius(12.0f)
                .transition(core::Transition::make(0.16f, core::Ease::OutCubic))
                .animate(core::AnimProperty::Color)
                .onClick([page] {
                    g_selectedPage = page;
                    g_dropdownOpen = false; // reset transient state when switching pages
                    saveSettings();
                })
                .build();

            ui.text(id + ".icon")
                .y(8.0f)
                .size(kSidebarWidth - 12.0f, 28.0f)
                .icon(icon)
                .fontSize(22.0f)
                .lineHeight(26.0f)
                .color(iconC)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .transition(core::Transition::make(0.18f, core::Ease::OutCubic))
                .animate(core::AnimProperty::TextColor)
                .build();

            ui.text(id + ".label")
                .y(38.0f)
                .size(kSidebarWidth - 12.0f, 18.0f)
                .text(label)
                .fontSize(11.0f)
                .lineHeight(14.0f)
                .color(labelC)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .transition(core::Transition::make(0.18f, core::Ease::OutCubic))
                .animate(core::AnimProperty::TextColor)
                .build();
        });
}

void composeSidebar(core::dsl::Ui& ui, float height) {
    ui.stack("sidebar")
        .size(kSidebarWidth, height)
        .content([&] {
            ui.rect("sidebar.bg")
                .size(kSidebarWidth, height)
                .color(sidebarBg())
                .build();

            // Animated accent bar on the left edge that slides to selected nav item
            const float accentY = kSidebarBrandH + kSidebarItemTop
                                + static_cast<float>(navOrderForPage(g_selectedPage)) * (kSidebarItemH + kSidebarItemGap);
            ui.rect("sidebar.accent")
                .x(0.0f)
                .y(kSidebarBrandH + kSidebarItemTop)
                .size(3.0f, kSidebarItemH)
                .color(accent())
                .radius(2.0f)
                .translateY(accentY - (kSidebarBrandH + kSidebarItemTop))
                .transition(core::Transition::make(0.28f, core::Ease::OutCubic))
                .animate(core::AnimProperty::Transform | core::AnimProperty::Color)
                .build();

            ui.column("sidebar.brand")
                .y(8.0f)
                .size(kSidebarWidth, kSidebarBrandH - 8.0f)
                .alignItems(core::Align::CENTER)
                .justifyContent(core::Align::CENTER)
                .gap(2.0f)
                .content([&] {
                    ui.text("brand.icon")
                        .size(kSidebarWidth, 30.0f)
                        .icon(0xF5FD)
                        .fontSize(24.0f)
                        .lineHeight(28.0f)
                        .color(accent())
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();
                    ui.text("brand.title")
                        .size(kSidebarWidth, 18.0f)
                        .text("EUI")
                        .fontSize(13.0f)
                        .fontWeight(700)
                        .color(textPri())
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .build();
                });

            ui.column("sidebar.nav")
                .x(6.0f)
                .y(kSidebarBrandH + kSidebarItemTop)
                .size(kSidebarWidth - 12.0f, height - kSidebarBrandH - kSidebarItemTop)
                .gap(kSidebarItemGap)
                .alignItems(core::Align::CENTER)
                .content([&] {
                    sidebarItem(ui, "nav.controls", "控件",   0xF1B2, 0);
                    sidebarItem(ui, "nav.input",    "输入",   0xF11C, 1);
                    sidebarItem(ui, "nav.picker",   "选择",   0xF073, 2);
                    sidebarItem(ui, "nav.chart",    "图表",   0xF080, 3);
                    sidebarItem(ui, "nav.clock",    "时钟",   0xF017, 4);
                    sidebarItem(ui, "nav.about",    "关于",   0xF05A, 5);
                });
        });
}

// ── Page 0: Controls (buttons, toggles, radios) ────────────────────
void pageControls(core::dsl::Ui& ui, float w) {
    const float cardW  = w - 2.0f * kContentPaddingX;
    const float innerW = cardW - 28.0f;

    sectionTitle(ui, "ctl", "按钮 Buttons", "三种样式 demo", cardW);
    cardSection(ui, "ctl.btn", cardW, 200.0f, [&] {
        components::button(ui, "btn.primary")
            .size(innerW, 44.0f)
            .text("主按钮 Primary")
            .colors(accent(), btnHover(accent()), btnPress(accent()))
            .radius(10.0f)
            .onClick([] { showToast("主按钮已点击"); })
            .build();

        components::button(ui, "btn.secondary")
            .size(innerW, 44.0f)
            .text("次按钮 Secondary")
            .colors(surface(), btnHover(surface()), btnPress(surface()))
            .textColor(textPri())
            .border(1.5f, borderC())
            .radius(10.0f)
            .onClick([] { showToast("Secondary 已点击"); })
            .build();

        components::button(ui, "btn.danger")
            .size(innerW, 44.0f)
            .text("打开对话框")
            .colors(dangerC(), btnHover(dangerC()), btnPress(dangerC()))
            .radius(10.0f)
            .onClick([] { g_dialogOpen = true; })
            .build();
    });

    sectionTitle(ui, "tog", "开关 Toggles", "Switch · Checkbox · Radio", cardW);
    cardSection(ui, "tog", cardW, 300.0f, [&] {
        ui.row("tog.sw1").size(innerW, 36.0f).alignItems(core::Align::CENTER)
            .content([&] {
                components::text(ui, "tog.sw1.lbl")
                    .size(innerW - 60.0f, 22.0f)
                    .text("Wi-Fi 无线网络")
                    .fontSize(14.0f).color(textPri()).build();
                components::toggleSwitch(ui, "sw.wifi")
                    .size(48.0f, 28.0f)
                    .theme(themeColors())
                    .checked(g_switchOn)
                    .onChange([](bool v) { g_switchOn = v; saveSettings(); })
                    .build();
            });

        ui.row("tog.sw2").size(innerW, 36.0f).alignItems(core::Align::CENTER)
            .content([&] {
                components::text(ui, "tog.sw2.lbl")
                    .size(innerW - 60.0f, 22.0f)
                    .text("深色模式")
                    .fontSize(14.0f).color(textPri()).build();
                components::toggleSwitch(ui, "sw.dark")
                    .size(48.0f, 28.0f)
                    .theme(themeColors())
                    .checked(g_switchDark)
                    .onChange([](bool v) { g_switchDark = v; saveSettings(); })
                    .build();
            });

        ui.row("tog.ca").size(innerW, 30.0f).alignItems(core::Align::CENTER)
            .content([&] {
                components::checkbox(ui, "cb.a").size(20.0f, 20.0f).theme(themeColors())
                    .checked(g_checkA).onChange([](bool v) { g_checkA = v; saveSettings(); }).build();
                components::text(ui, "tog.ca.lbl")
                    .size(innerW - 30.0f, 20.0f)
                    .margin(10.0f, 0.0f, 0.0f, 0.0f)
                    .text("接受用户协议").fontSize(13.0f).color(textPri()).build();
            });

        ui.row("tog.cb").size(innerW, 30.0f).alignItems(core::Align::CENTER)
            .content([&] {
                components::checkbox(ui, "cb.b").size(20.0f, 20.0f).theme(themeColors())
                    .checked(g_checkB).onChange([](bool v) { g_checkB = v; saveSettings(); }).build();
                components::text(ui, "tog.cb.lbl")
                    .size(innerW - 30.0f, 20.0f)
                    .margin(10.0f, 0.0f, 0.0f, 0.0f)
                    .text("订阅邮件").fontSize(13.0f).color(textPri()).build();
            });

        ui.row("tog.cc").size(innerW, 30.0f).alignItems(core::Align::CENTER)
            .content([&] {
                components::checkbox(ui, "cb.c").size(20.0f, 20.0f).theme(themeColors())
                    .checked(g_checkC).onChange([](bool v) { g_checkC = v; saveSettings(); }).build();
                components::text(ui, "tog.cc.lbl")
                    .size(innerW - 30.0f, 20.0f)
                    .margin(10.0f, 0.0f, 0.0f, 0.0f)
                    .text("启用通知").fontSize(13.0f).color(textPri()).build();
            });

        // Radios — give each enough width so the dot doesn't overlap the next
        // option's label. Using flow lets them wrap if container is narrow.
        ui.flow("tog.rd").size(innerW, 36.0f).gap(8.0f)
            .content([&] {
                components::radio(ui, "rd.a").size(78.0f, 30.0f).fontSize(14.0f).theme(themeColors())
                    .text("浅色").selected(g_radioSelect == 0)
                    .onSelect([] { g_radioSelect = 0; saveSettings(); }).build();
                components::radio(ui, "rd.b").size(78.0f, 30.0f).fontSize(14.0f).theme(themeColors())
                    .text("深色").selected(g_radioSelect == 1)
                    .onSelect([] { g_radioSelect = 1; saveSettings(); }).build();
                components::radio(ui, "rd.c").size(78.0f, 30.0f).fontSize(14.0f).theme(themeColors())
                    .text("自动").selected(g_radioSelect == 2)
                    .onSelect([] { g_radioSelect = 2; saveSettings(); }).build();
            });
    });
}

// ── Page 1: Input (sliders, text, stepper) ─────────────────────────
void pageInput(core::dsl::Ui& ui, float w) {
    const float cardW  = w - 2.0f * kContentPaddingX;
    const float innerW = cardW - 28.0f;

    sectionTitle(ui, "sl", "滑块 Sliders", "音量与亮度", cardW);
    cardSection(ui, "sl", cardW, 200.0f, [&] {
        components::text(ui, "sl.lbl1")
            .size(innerW, 20.0f)
            .text("音量 Volume  " + std::to_string((int)(g_slider1 * 100)) + "%")
            .fontSize(13.0f).color(textSec())
            .horizontalAlign(core::HorizontalAlign::Center).build();
        components::slider(ui, "sl.s1").size(innerW, 32.0f).theme(themeColors())
            .value(g_slider1).onChange([](float v) { g_slider1 = v; saveSettings(); }).build();

        components::text(ui, "sl.lbl2")
            .size(innerW, 20.0f)
            .text("亮度 Brightness  " + std::to_string((int)(g_slider2 * 100)) + "%")
            .fontSize(13.0f).color(textSec())
            .horizontalAlign(core::HorizontalAlign::Center).build();
        components::slider(ui, "sl.s2").size(innerW, 32.0f).theme(themeColors())
            .value(g_slider2).onChange([](float v) { g_slider2 = v; saveSettings(); }).build();

        components::progress(ui, "sl.prog").size(innerW, 8.0f).theme(themeColors()).value(g_progress).build();
    });

    sectionTitle(ui, "inp", "文本输入 Text Input", "点击弹出软键盘", cardW);
    cardSection(ui, "inp", cardW, 160.0f, [&] {
        components::input(ui, "inp.en").size(innerW, 44.0f).theme(themeColors())
            .placeholder("Type something...")
            .text(g_input).onChange([](const std::string& v) { g_input = v; saveSettings(); }).build();

        components::input(ui, "inp.cn").size(innerW, 44.0f).theme(themeColors())
            .placeholder("中文输入测试")
            .text(g_inputCn).onChange([](const std::string& v) { g_inputCn = v; saveSettings(); }).build();
    });

    sectionTitle(ui, "seg", "分段与步进 Segmented / Tabs / Stepper", "", cardW);
    cardSection(ui, "seg", cardW, 200.0f, [&] {
        components::segmented(ui, "seg.view").size(innerW, 38.0f).theme(themeColors())
            .items({"列表", "网格", "日历"})
            .selected(g_segment)
            .onChange([](int idx) { g_segment = idx; saveSettings(); }).build();

        components::tabs(ui, "tabs.view").size(innerW, 38.0f).theme(themeColors())
            .items({"详情", "评论", "推荐"})
            .selected(g_tab)
            .onChange([](int idx) { g_tab = idx; saveSettings(); }).build();

        components::stepper(ui, "step.qty").size(innerW, 42.0f).theme(themeColors())
            .value(g_stepper).min(0).max(99).step(1)
            .onChange([](long long v) { g_stepper = (int)v; saveSettings(); }).build();
    });
}

// ── Page 2: Pickers (dropdown, date, time, color) ──────────────────
void pagePickers(core::dsl::Ui& ui, float w) {
    const float cardW  = w - 2.0f * kContentPaddingX;
    const float innerW = cardW - 28.0f;

    sectionTitle(ui, "dd", "下拉框 Dropdown", "点击展开,展开内容会浮在下方控件之上", cardW);
    // Card stays a fixed slim height — the field is all that takes layout
    // space. When the dropdown opens, its popup overflows below the card
    // and overlays the next sections (high zIndex below ensures the popup
    // is drawn on top, not behind them).
    cardSection(ui, "dd", cardW, 80.0f, [&] {
        components::dropdown(ui, "dd.country").size(innerW, 44.0f).theme(themeColors())
            .items({"中国 China", "美国 USA", "日本 Japan", "韩国 Korea", "德国 Germany"})
            .selected(g_dropdown)
            .open(g_dropdownOpen)
            .onChange([](int idx) {
                g_dropdown = idx;
                saveSettings();
            })
            .onOpenChange([](bool open) {
                g_dropdownOpen = open;
            })
            .build();
    }, /*zIndex=*/ 50);

    sectionTitle(ui, "pk", "日期时间颜色 Pickers", "Date · Time · Color", cardW);
    cardSection(ui, "pk", cardW, 230.0f, [&] {
        char dateBuf[32];
        std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", g_year, g_month, g_day);
        components::button(ui, "btn.date").size(innerW, 44.0f)
            .text(std::string("日期 ") + dateBuf)
            .colors(surface(), btnHover(surface()), btnPress(surface()))
            .textColor(textPri()).border(1.0f, borderC()).radius(10.0f)
            .onClick([] { g_datePickerOpen = true; }).build();

        char timeBuf[16];
        std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", g_hour, g_minute);
        components::button(ui, "btn.time").size(innerW, 44.0f)
            .text(std::string("时间 ") + timeBuf)
            .colors(surface(), btnHover(surface()), btnPress(surface()))
            .textColor(textPri()).border(1.0f, borderC()).radius(10.0f)
            .onClick([] { g_timePickerOpen = true; }).build();

        components::button(ui, "btn.color").size(innerW, 44.0f)
            .text("主题色 Theme color")
            .colors(g_pickedColor, btnHover(g_pickedColor), btnPress(g_pickedColor))
            .radius(10.0f)
            .onClick([] { g_colorPickerOpen = true; }).build();
    });
}

// ── Page 3: Charts ────────────────────────────────────────────────
void pageCharts(core::dsl::Ui& ui, float w) {
    const float cardW  = w - 2.0f * kContentPaddingX;
    const float innerW = cardW - 28.0f;

    // Make the chart data ripple over time so they're not static. Both
    // lineChart and barChart clamp values to [0.0, 1.0] internally (treating
    // them as percentages), so the wave needs to stay inside that range —
    // otherwise everything reads as 100% and you get a flat line / equal bars.
    const float t = static_cast<float>(g_tick) * 0.10f;
    std::vector<float> line(7);
    for (int i = 0; i < 7; ++i) {
        line[i] = 0.50f + 0.30f * std::sin(t + i * 0.55f)
                       + 0.12f * std::cos(t * 0.6f + i * 1.1f);
    }
    std::vector<float> bar(6);
    for (int i = 0; i < 6; ++i) {
        bar[i] = 0.45f + 0.28f * std::sin(t * 0.8f + i * 0.9f + 1.2f)
                      + 0.12f * std::cos(t * 1.4f + i * 0.3f);
    }
    // Pie normalizes by sum internally, so absolute scale doesn't matter here.
    std::vector<float> pie(5);
    for (int i = 0; i < 5; ++i) {
        pie[i] = 20.0f + 8.0f * std::sin(t * 0.5f + i * 1.3f);
    }

    sectionTitle(ui, "ln", "折线图 Line Chart", "数据每 100ms 刷新", cardW);
    cardSection(ui, "ln", cardW, 200.0f, [&] {
        components::lineChart(ui, "ch.line").size(innerW, 160.0f)
            .values(line)
            .theme(themeColors()).build();
    });

    sectionTitle(ui, "br", "柱状图 Bar Chart", "", cardW);
    cardSection(ui, "br", cardW, 200.0f, [&] {
        components::barChart(ui, "ch.bar").size(innerW, 160.0f)
            .values(bar)
            .theme(themeColors()).build();
    });

    sectionTitle(ui, "pi", "饼图 Pie Chart", "", cardW);
    cardSection(ui, "pi", cardW, 220.0f, [&] {
        components::pieChart(ui, "ch.pie").size(180.0f, 180.0f)
            .values(pie)
            .theme(themeColors()).build();
    });

    // Hidden timer that re-arms itself every compose. Each fire bumps g_tick
    // and forces another compose, which re-runs this branch and recomputes
    // the chart values from the new tick. ~100ms period = 10Hz ripple.
    ui.stack("ch.tick").size(0.0f, 0.0f)
        .onTimer(0.10f, [] { ++g_tick; })
        .build();
}

// ── Page 4: Clock ─────────────────────────────────────────────────
void pageClock(core::dsl::Ui& ui, float w) {
    const float cardW  = w - 2.0f * kContentPaddingX;
    const float innerW = cardW - 28.0f;
    constexpr float kPi = 3.14159265358979323846f;

    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif

    char timeBuf[16];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                  local.tm_hour, local.tm_min, local.tm_sec);
    char dateBuf[32];
    std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

    sectionTitle(ui, "ck", "时钟 Analog Clock", timeBuf, cardW);

    const float clockH = std::min(innerW + 60.0f, 320.0f);
    cardSection(ui, "ck", cardW, clockH, [&] {
        const float size  = std::min(innerW, clockH - 40.0f);
        const float cx    = size * 0.5f;
        const float cy    = size * 0.5f;
        const float r     = size * 0.5f;

        ui.stack("ck.face").size(size, size).content([&] {
            // Outer face — pulls from theme so the clock matches light/dark.
            ui.rect("ck.face.bg").size(size, size)
                .color(surface())
                .radius(r)
                .border(1.5f, borderC()).build();

            // Inner ring — slightly different shade than the outer face.
            ui.rect("ck.face.inner")
                .x(r * 0.18f).y(r * 0.18f)
                .size(size - r * 0.36f, size - r * 0.36f)
                .color(surface2())
                .radius(r * 0.82f).build();

            // 12 ticks around the face
            for (int i = 0; i < 12; ++i) {
                const float angle = i * kPi / 6.0f;
                const bool major  = (i % 3 == 0);
                const float tw    = major ? 4.0f : 2.0f;
                const float th    = major ? 14.0f : 8.0f;
                const float tx    = cx + std::sin(angle) * (r - 16.0f) - tw * 0.5f;
                const float ty    = cy - std::cos(angle) * (r - 16.0f) - th * 0.5f;
                ui.rect("ck.tick." + std::to_string(i))
                    .x(tx).y(ty)
                    .size(tw, th)
                    .color(major ? textPri() : textSec())
                    .radius(tw * 0.5f)
                    .rotate(angle)
                    .transformOrigin(0.5f, 0.5f)
                    .build();
            }

            // Clock hands — each hand is a rectangle rotated so its tail sits
            // at the center pin. We rotate around the hand's pivot point.
            auto hand = [&](const char* id, float angle, float width,
                            float length, core::Color color) {
                const float tail = length * 0.14f;
                ui.rect(id)
                    .x(cx - width * 0.5f)
                    .y(cy - length + tail)
                    .size(width, length)
                    .color(color)
                    .radius(width * 0.5f)
                    .rotate(angle)
                    .transformOrigin(0.5f, (length - tail) / length)
                    .build();
            };

            const float hourAng   = (local.tm_hour % 12 + local.tm_min / 60.0f) * kPi / 6.0f;
            const float minuteAng = (local.tm_min + local.tm_sec / 60.0f) * kPi / 30.0f;
            const float secondAng = local.tm_sec * kPi / 30.0f;
            hand("ck.hour",   hourAng,   6.0f, r * 0.48f, textPri());
            hand("ck.minute", minuteAng, 4.0f, r * 0.70f, textPri());
            hand("ck.second", secondAng, 2.0f, r * 0.78f, {0.95f, 0.30f, 0.35f, 1.0f});

            // Center pin
            ui.rect("ck.pin")
                .x(cx - 5.0f).y(cy - 5.0f)
                .size(10.0f, 10.0f)
                .color(accent())
                .radius(5.0f).build();
        });

        components::text(ui, "ck.date")
            .size(innerW, 22.0f)
            .text(dateBuf)
            .fontSize(13.0f).color(textSec())
            .horizontalAlign(core::HorizontalAlign::Center).build();
    });

    // Tick every 0.25s so the second hand moves smoothly. (The runtime's
    // animation interpolator won't carry transforms between composes here,
    // so this is just discrete updates — 4Hz is plenty for a clock.)
    ui.stack("ck.tick").size(0.0f, 0.0f)
        .onTimer(0.25f, [] { ++g_tick; })
        .build();
}

// ── Page 4: About ─────────────────────────────────────────────────
void pageAbout(core::dsl::Ui& ui, float w) {
    const float cardW = w - 2.0f * kContentPaddingX;

    sectionTitle(ui, "abt", "关于 About", "EUI-NEO mobile demo", cardW);
    cardSection(ui, "abt", cardW, 200.0f, [&] {
        components::text(ui, "abt.title")
            .size(cardW - 28.0f, 26.0f)
            .text("EUI-NEO")
            .fontSize(20.0f).fontWeight(800).color(textPri())
            .horizontalAlign(core::HorizontalAlign::Center).build();

        components::text(ui, "abt.sub")
            .size(cardW - 28.0f, 22.0f)
            .text("C++17 跨平台 UI 框架")
            .fontSize(13.0f).color(textSec())
            .horizontalAlign(core::HorizontalAlign::Center).build();

        components::text(ui, "abt.body")
            .size(cardW - 28.0f, 80.0f)
            .text("基于 OpenGL ES + GLFW shim,\n"
                  "在 Android 上以 EGL 渲染。\n"
                  "Built on OpenGL ES with a custom\n"
                  "GLFW shim, EGL-backed on Android.")
            .fontSize(12.0f).lineHeight(18.0f).color(textSec())
            .horizontalAlign(core::HorizontalAlign::Center).build();
    });
}

void composeRightPanel(core::dsl::Ui& ui, float width, float height) {
    components::scrollView(ui, "page.scroll")
        .size(width, height)
        .theme(themeColors())
        .offset(g_pageScroll[g_selectedPage])
        .onChange([](float v) { g_pageScroll[g_selectedPage] = v; })
        .content([&](core::dsl::Ui& ui, float contentWidth, float viewportHeight) {
            (void)viewportHeight;
            ui.column("page.col")
                .width(contentWidth)
                .height(core::SizeValue::wrapContent())
                .padding(kContentPaddingX, 12.0f, kContentPaddingX, 30.0f)
                .gap(0.0f)
                .alignItems(core::Align::CENTER)
                .content([&] {
                    switch (g_selectedPage) {
                        case 0: pageControls(ui, contentWidth); break;
                        case 1: pageInput(ui, contentWidth);    break;
                        case 2: pagePickers(ui, contentWidth);  break;
                        case 3: pageCharts(ui, contentWidth);   break;
                        case 4: pageClock(ui, contentWidth);    break;
                        case 5: pageAbout(ui, contentWidth);    break;
                        default: break;
                    }
                });
        })
        .build();
}

} // namespace

// ── Config ────────────────────────────────────────────────────────
const DslAppConfig& dslAppConfig() {
    static DslAppConfig config = DslAppConfig{}
        .title("EUI-NEO Demo")
        .pageId("main")
        .clearColor(appBg())
        .windowSize(390, 844)
        .fps(60.0)
        .textFont(eui_android_text_font_path() ? eui_android_text_font_path() : "")
        .iconFont(eui_android_icon_font_path() ? eui_android_icon_font_path() : "")
        .showFrameCountInTitle(false);
    return config;
}

// ── Compose ───────────────────────────────────────────────────────
void compose(core::dsl::Ui& ui, const core::dsl::Screen& screen) {
    // Restore persisted user state on the first compose after launch.
    // fontsDir is populated by extractFontAssets in nativeMain JNI, which runs
    // before the native main loop calls compose, so the path is ready.
    static bool s_settingsLoaded = false;
    if (!s_settingsLoaded) {
        loadSettings();
        s_settingsLoaded = true;
    }

    ui.row("layout")
        .size(screen.width, screen.height)
        .alignItems(core::Align::START)
        .content([&] {
            composeSidebar(ui, screen.height);
            ui.stack("content")
                .size(screen.width - kSidebarWidth, screen.height)
                .content([&] {
                    // Background rect so the page area picks up the current
                    // theme's background color (the static glClearColor in
                    // dslAppConfig() can't follow runtime theme switches).
                    ui.rect("content.bg")
                        .size(screen.width - kSidebarWidth, screen.height)
                        .color(appBg())
                        .build();
                    composeRightPanel(ui, screen.width - kSidebarWidth, screen.height);
                });
        });

    // ── Overlays — top-level so they aren't clipped by the page scroll ──
    components::dialog(ui, "dlg.confirm")
        .theme(themeColors())
        .screen(screen.width, screen.height)
        .size(320.0f, 200.0f)
        .open(g_dialogOpen)
        .title("确认操作")
        .message("你确定要执行此操作吗?\nAre you sure you want to proceed?")
        .primaryText("确定 OK")
        .secondaryText("取消 Cancel")
        .onPrimary([] { g_dialogOpen = false; showToast("操作已确认"); })
        .onSecondary([] { g_dialogOpen = false; showToast("操作已取消"); })
        .onClose([] { g_dialogOpen = false; })
        .build();

    components::datepicker(ui, "dp.cal")
        .theme(themeColors())
        .screen(screen.width, screen.height)
        .open(g_datePickerOpen)
        .date(g_year, g_month, g_day)
        .onChange([](int y, int m, int d) { g_year = y; g_month = m; g_day = d; saveSettings(); })
        .onOpenChange([](bool open) { g_datePickerOpen = open; })
        .build();

    components::timepicker(ui, "tp.clock")
        .theme(themeColors())
        .screen(screen.width, screen.height)
        .open(g_timePickerOpen)
        .time(g_hour, g_minute)
        .onChange([](int h, int m) { g_hour = h; g_minute = m; saveSettings(); })
        .onOpenChange([](bool open) { g_timePickerOpen = open; })
        .build();

    components::colorpicker(ui, "cp.color")
        .theme(themeColors())
        .screen(screen.width, screen.height)
        .open(g_colorPickerOpen)
        .value(g_pickedColor)
        .onChange([](const core::Color& c) { g_pickedColor = c; saveSettings(); })
        .onOpenChange([](bool open) { g_colorPickerOpen = open; })
        .build();

    components::toast(ui, "toast.fb")
        .theme(themeColors())
        .screen(screen.width, screen.height)
        .visible(g_toastVisible)
        .duration(4.5f)
        .message(g_toastMsg)
        .onAutoDismiss([] { g_toastVisible = false; })
        .onDismiss([] { g_toastVisible = false; })
        .build();
}

} // namespace app
