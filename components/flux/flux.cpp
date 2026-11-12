// LichtValley Flux: solar, battery, house and grid — the Power-Flux-Look. Based on hflocki/tessera-energy (MIT),
// extended with the battery: its power between battery and house, its state of charge in the battery's label, and a
// battery row on the card.
#include "flux.h"
#include "esphome/core/log.h"

namespace esphome::flux {

static const char *const TAG = "flux";

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// ---- Readable colours ---------------------------------------------------------------------------------------------
// A colour's luminance and the WCAG contrast of two: amber or yellow words on the light look's white card fall under
// what a person reads, so there they take the ink instead; on the dark look they keep their colour.
static float luminance(lv_color_t c) {
    auto channel = [](uint8_t v) {
        const float s = v / 255.0f;
        return s <= 0.03928f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * channel(c.red) + 0.7152f * channel(c.green) + 0.0722f * channel(c.blue);
}
static bool reads_on_card(lv_color_t c) {
    const float a = luminance(c), b = luminance(ui::color(theme::CARD));
    return (std::max(a, b) + 0.05f) / (std::min(a, b) + 0.05f) >= 3.0f;
}
// A label (words or an icon) in a tone's or a role's colour where that reads on the card, else in the ink.
static void set_readable(lv_obj_t *label, tessera::Tone tone, theme::Role role) {
    if (reads_on_card(ui::tone_color(tone, role))) ui::set_tone(label, tone, role);
    else ui::set_color(label, theme::INK);
}
static constexpr uint32_t ICON_SOLAR = 0xF0A72;   // solar-power
static constexpr uint32_t ICON_HOME = 0xF02DC;    // home
static constexpr uint32_t ICON_GRID = 0xF0D3E;    // transmission-tower
static constexpr uint32_t ICON_BATTERY = 0xF0079; // battery
static constexpr uint32_t ICON_RIGHT = 0xF0054;   // arrow-right
static constexpr uint32_t ICON_LEFT = 0xF004D;    // arrow-left
static constexpr float FLOWING = 5.0f;            // watts below which nothing counts as flowing

// The colours of the four: the sun amber, the grid blue while it gives and green while it takes, the battery green
// while it charges and amber while it gives.
static constexpr theme::Role SOLAR = theme::MARK_AMBER;
static constexpr theme::Role IMPORT = theme::MARK_BLUE;
static constexpr theme::Role EXPORT = theme::MARK_GREEN;

// A label inside a circle: a child of it, so the two are one part, centred in it.
static void inside(lv_obj_t *circle, lv_obj_t *label, int w, int h, int dy = 0) {
    lv_obj_set_parent(label, circle);
    const int cw = lv_obj_get_width(circle), ch = lv_obj_get_height(circle);
    lv_obj_set_pos(label, (cw - w) / 2, (ch - h) / 2 + dy);
    lv_obj_set_size(label, std::max(1, w), std::max(1, h));
}

// ---- Reading the sensors ------------------------------------------------------------------------------------------

static bool known(const std::string &s) { return !s.empty() && s != "unknown" && s != "unavailable" && s != "None"; }

float Flux::number(int index) const {
    const text_sensor::TextSensor *s = fields_[index];
    if (s == nullptr || !known(s->state)) return NAN;
    char *end = nullptr;
    const float v = strtof(s->state.c_str(), &end);
    return end == s->state.c_str() ? NAN : v;
}

const char *Flux::unit(int index) const {
    const text_sensor::TextSensor *s = fields_[index];
    return s == nullptr ? "" : s->state.c_str();
}

// Power in W whatever the sensor's unit (W, kW, MW).
static float watts(float v, const char *unit) {
    if (std::isnan(v)) return NAN;
    const std::string u = unit;
    float f = 1.0f;
    if (u == "kW" || u == "k") f = 1000.0f;
    else if (u == "MW" || u == "M") f = 1000000.0f;
    return v * f;
}

// Energy in kWh whatever the sensor's unit (Wh, kWh, MWh).
static float kwh(float v, const char *unit) {
    if (std::isnan(v)) return NAN;
    const std::string u = unit;
    float f = 1.0f;
    if (u == "Wh" || u == "W") f = 0.001f;
    else if (u == "MWh" || u == "M") f = 1000.0f;
    return v * f;
}

Reading Flux::read(bool invert, bool bat_invert) const {
    Reading r;
    r.grid = watts(number(F_GRID), unit(F_GRID_UNIT));
    r.have_grid = !std::isnan(r.grid);
    if (r.have_grid && invert) r.grid = -r.grid;
    r.pv = watts(number(F_PV), unit(F_PV_UNIT));
    if (!std::isnan(r.pv)) r.pv = std::max(0.0f, r.pv);
    r.bat = watts(number(F_BAT_RATE), unit(F_BAT_RATE_UNIT));
    r.have_bat = !std::isnan(r.bat);
    if (r.have_bat && bat_invert) r.bat = -r.bat;
    if (r.have_grid) r.home = std::max(0.0f, r.grid + (std::isnan(r.pv) ? 0.0f : r.pv) + (r.have_bat ? r.bat : 0.0f));
    r.coverage = number(F_COVERAGE);
    if (std::isnan(r.coverage) && !std::isnan(r.pv) && !std::isnan(r.home))
        r.coverage = r.home > FLOWING ? 100.0f * std::min(r.pv, r.home) / r.home : (r.pv > FLOWING ? 100.0f : 0.0f);
    if (!std::isnan(r.coverage)) r.coverage = std::max(0.0f, std::min(100.0f, r.coverage));
    r.pv_today = kwh(number(F_PV_TODAY), unit(F_PV_TODAY_UNIT));
    r.forecast = kwh(number(F_FORECAST), unit(F_FORECAST_UNIT));
    r.grid_today = kwh(number(F_GRID_TODAY), unit(F_GRID_TODAY_UNIT));
    r.soc = number(F_SOC);
    if (!std::isnan(r.soc)) r.soc = std::max(0.0f, std::min(100.0f, r.soc));
    return r;
}

// "1.24" with the screen's decimal sign.
static std::string decimal(std::string text, const char *sign) {
    if (sign && *sign && *sign != '.')
        for (char &c : text)
            if (c == '.') c = *sign;
    return text;
}

std::string Flux::power_text(float w) const {
    if (std::isnan(w)) return "–";
    char buf[24];
    const float a = std::fabs(w);
    if (a < 1000.0f) snprintf(buf, sizeof buf, "%d W", static_cast<int>(lroundf(a)));
    else snprintf(buf, sizeof buf, a < 10000.0f ? "%.2f kW" : "%.1f kW", a / 1000.0f);
    return decimal(buf, text("decimal"));
}

std::string Flux::energy_text(float v) const {
    if (std::isnan(v)) return "–";
    char buf[24];
    snprintf(buf, sizeof buf, v < 100.0f ? "%.1f kWh" : "%.0f kWh", v);
    return decimal(buf, text("decimal"));
}

// ---- Drawing helpers ----------------------------------------------------------------------------------------------

static void place(lv_obj_t *obj, int x, int y, int w, int h) {
    lv_obj_set_size(obj, std::max(1, w), std::max(1, h));
    lv_obj_set_pos(obj, x, y);
}

// Opacities written only when they differ: a write redraws the object, so not on every tick.
static void set_bg_opa(lv_obj_t *obj, lv_opa_t opa) {
    if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(obj, opa, 0);
}
// Shown or hidden, only when they differ. A hidden part is not drawn and takes no room.
static void show(lv_obj_t *obj, bool on) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == on) {
        if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static int percent(float v) { return static_cast<int>(lroundf(v)); }

// ---- The view: the tile's and the card's drawing ------------------------------------------------------------------

// The four (solar, battery, house, grid) side by side with arrows where the power flows, a bar of how much of the
// house's use the sun covers, and the day so far. Without a battery three, as hflocki's energy_flow. Narrow, it
// shows the grid, the sun and the battery in lines under each other. Each part only while there is room for it and a
// value to show.
class FlowView {
 public:
    explicit FlowView(const Flux *plugin) : plugin_(plugin) {}

    void create(lv_obj_t *parent, int width, int height, bool today, bool large) {
        width_ = width;
        height_ = height;
        today_ = today;
        label_h_ = ui::line_height(Font::BODY);
        value_font_ = large && ui::large() ? Font::VALUE : Font::TITLE;
        value_h_ = ui::line_height(value_font_);
        body_h_ = label_h_;
        gap_ = ui::px(ui::large() ? (large ? 14 : 8) : 5);
        bar_h_ = ui::px(large ? 10 : 6);
        arrow_w_ = ui::line_height(Font::ICON_SMALL);

        const int text_h = label_h_ + value_h_;
        dia_ = std::min(ui::mm(large ? 16 : 12), std::max(ui::line_height(Font::ICON_SMALL) + ui::px(8), text_h));
        dia_ = std::min(dia_, height_);
        big_icon_ = dia_ >= ui::line_height(Font::ICON) + ui::px(10);
        main_h_ = std::max(dia_, text_h);
        // The four circles stand in a row with their values and labels under them: they fit when the circles and
        // the arrows between them span the width.
        row_h_ = dia_ + value_h_ + label_h_ + ui::px(6);
        wide_ = 4 * dia_ + 3 * (arrow_w_ + ui::px(8)) + ui::px(16) <= width_;

        for (Block &b : blocks_) {
            b.circle = ui::block(parent, theme::TRACK);
            lv_obj_set_style_radius(b.circle, LV_RADIUS_CIRCLE, 0);
            b.icon = ui::label(b.circle, big_icon_ ? Font::ICON : Font::ICON_SMALL, theme::MUTED);
            lv_obj_set_style_text_align(b.icon, LV_TEXT_ALIGN_CENTER, 0);
            b.label = ui::label(parent, Font::BODY, theme::MUTED);
            b.value = ui::label(parent, value_font_, theme::INK);
        }
        for (lv_obj_t *&a : arrows_) {
            a = ui::label(parent, Font::ICON_SMALL, theme::MUTED);
            lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
        }
        share_track_ = ui::block(parent, theme::TRACK);
        share_sun_ = ui::block(parent, SOLAR);
        share_grid_ = ui::block(parent, IMPORT);
        share_left_ = ui::label(parent, Font::BODY, theme::MUTED);
        share_right_ = ui::label(parent, Font::BODY, theme::MUTED);
        lv_obj_set_style_text_align(share_right_, LV_TEXT_ALIGN_RIGHT, 0);
        day_ = ui::label(parent, Font::BODY, theme::INK_SOFT);
        fc_track_ = ui::block(parent, theme::TRACK);
        fc_fill_ = ui::block(parent, SOLAR);
        fc_label_ = ui::label(parent, Font::BODY, theme::MUTED);
        note_ = ui::label(parent, Font::BODY_LARGE, theme::MUTED);
        place(note_, 0, (height_ - ui::line_height(Font::BODY_LARGE)) / 2, width_, ui::line_height(Font::BODY_LARGE));
        // Nothing shows until the first paint has placed it.
        for (Block &b : blocks_) hide_block(b);
        for (lv_obj_t *o : {arrows_[0], arrows_[1], arrows_[2], share_track_, share_sun_, share_grid_, share_left_,
                            share_right_, day_, fc_track_, fc_fill_, fc_label_})
            show(o, false);
    }

    void paint(const Reading &r) {
        if (!r.have_grid) {
            for (Block &b : blocks_) hide_block(b);
            for (lv_obj_t *o : {arrows_[0], arrows_[1], arrows_[2], share_track_, share_sun_, share_grid_, share_left_,
                                share_right_, day_, fc_track_, fc_fill_, fc_label_})
                show(o, false);
            show(note_, true);
            ui::set_text(note_, plugin_->text("choose"));
            ui::set_color(note_, theme::MUTED);
            shown_ = -1;
            return;
        }
        ui::set_text(note_, "");
        show(note_, false);

        // Which parts there is something for, and of those which fit, in this order: the share bar, the day, the
        // forecast. A change of them places everything again.
        const bool has_pv = !std::isnan(r.pv);
        const bool has_bat = r.have_bat;
        const std::string day = day_text(r);
        const bool want_share = !std::isnan(r.coverage) && r.home > FLOWING;
        const bool want_day = today_ && !day.empty();
        const bool want_fc = today_ && !std::isnan(r.forecast) && r.forecast > 0 && !std::isnan(r.pv_today);
        int parts = 0, used = wide_ ? row_h_ : compact_main_h(has_pv);
        auto take = [&](bool want, int h, int bit) {
            if (want && used + gap_ + h <= height_) {
                used += gap_ + h;
                parts |= bit;
            }
        };
        take(want_share, share_h(), 1);
        take(want_day, body_h_, 2);
        take(want_fc, bar_h_ + ui::px(4) + body_h_, 4);
        const int layout = parts | (has_pv ? 8 : 0) | (has_bat ? 16 : 0);
        if (layout != shown_) {
            shown_ = layout;
            arrange(parts, has_pv, has_bat, used);
        }

        paint_blocks(r, has_pv, has_bat);
        paint_share(r, parts & 1);
        show(day_, parts & 2);
        ui::set_text(day_, parts & 2 ? day : "");
        ui::set_color(day_, theme::INK_SOFT);
        paint_forecast(r, parts & 4);
    }

    void on_theme() {
        for (Block &b : blocks_) ui::set_color(b.label, theme::MUTED);
        ui::set_color(share_left_, theme::MUTED);
        ui::set_color(share_right_, theme::MUTED);
        ui::set_color(fc_label_, theme::MUTED);
        ui::set_fill(share_track_, Tone::NORMAL, theme::TRACK);
        ui::set_fill(fc_track_, Tone::NORMAL, theme::TRACK);
    }

 private:
    struct Block {
        lv_obj_t *circle{}, *icon{}, *label{}, *value{};
    };
    enum { SUN, BATTERY, HOME, GRID };

    int share_h() const { return bar_h_ + ui::px(4) + body_h_; }
    // Narrow: the grid's circle and value, the sun's and the battery's lines under it.
    int compact_main_h(bool has_pv) const { return main_h_ + (has_pv ? body_h_ : 0); }

    // Places every part for the parts shown, the whole block in the middle of the height.
    void arrange(int parts, bool has_pv, bool has_bat, int used) {
        int y = std::max(0, (height_ - used) / 2);
        const int text_h = label_h_ + value_h_;
        const int ih = ui::line_height(big_icon_ ? Font::ICON : Font::ICON_SMALL);
        auto put_block = [&](Block &b, int x, int w) {
            place(b.circle, x, y + (main_h_ - dia_) / 2, dia_, dia_);
            inside(b.circle, b.icon, dia_, ih);
            const int tx = x + dia_ + ui::px(8), tw = std::max(1, w - dia_ - ui::px(8));
            place(b.label, tx, y + (main_h_ - text_h) / 2, tw, label_h_);
            place(b.value, tx, y + (main_h_ - text_h) / 2 + label_h_, tw, value_h_);
        };
        const int nodes = has_bat ? 4 : 3;
        if (wide_) {
            // Four columns (three without a battery): each circle centred in its column, its value and label under
            // it; the arrows between the circles, at their mid-height.
            const int col = width_ / nodes;
            const int ih = ui::line_height(big_icon_ ? Font::ICON : Font::ICON_SMALL);
            int slot = 0;
            for (int i : has_bat ? std::vector<int>{SUN, BATTERY, HOME, GRID} : std::vector<int>{SUN, HOME, GRID}) {
                Block &b = blocks_[i];
                const int cx = slot * col + col / 2;
                place(b.circle, cx - dia_ / 2, y, dia_, dia_);
                inside(b.circle, b.icon, dia_, ih);
                place(b.value, slot * col + ui::px(4), y + dia_ + ui::px(4), col - ui::px(8), value_h_);
                place(b.label, slot * col, y + dia_ + ui::px(4) + value_h_, col, label_h_);
                lv_obj_set_style_text_align(b.value, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_align(b.label, LV_TEXT_ALIGN_CENTER, 0);
                ++slot;
            }
            const int ah = ui::line_height(Font::ICON_SMALL);
            for (int i = 0; i < nodes - 1; ++i)
                place(arrows_[i], (i + 1) * col - arrow_w_ / 2, y + dia_ / 2 - ah / 2, arrow_w_, ah);
            for (int i = nodes - 1; i < 3; ++i) show(arrows_[i], false);
            y += row_h_;
        } else {
            lv_obj_set_style_text_align(blocks_[GRID].value, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_align(blocks_[GRID].label, LV_TEXT_ALIGN_LEFT, 0);
            // The grid's block over the whole width, the sun's and the battery's lines under it; no house, no arrows.
            put_block(blocks_[GRID], 0, width_);
            lv_obj_set_style_text_align(blocks_[GRID].value, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_align(blocks_[GRID].label, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_align(blocks_[SUN].label, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_align(blocks_[BATTERY].label, LV_TEXT_ALIGN_LEFT, 0);
            hide_block(blocks_[SUN]);
            hide_block(blocks_[HOME]);
            hide_block(blocks_[BATTERY]);
            y += main_h_;
            if (has_pv) {
                place(blocks_[SUN].label, 0, y, width_, body_h_);
                y += body_h_;
            }
        }
        if (parts & 1) {
            y += gap_;
            share_y_ = y;
            place(share_track_, 0, y, width_, bar_h_);
            const int lw = width_ * 2 / 3;
            place(share_left_, 0, y + bar_h_ + ui::px(4), lw, body_h_);
            place(share_right_, width_ - (width_ - lw), y + bar_h_ + ui::px(4), width_ - lw, body_h_);
            y += share_h();
        }
        if (parts & 2) {
            y += gap_;
            place(day_, 0, y, width_, body_h_);
            y += body_h_;
        }
        if (parts & 4) {
            y += gap_;
            fc_y_ = y;
            place(fc_track_, 0, y, width_, bar_h_);
            place(fc_label_, 0, y + bar_h_ + ui::px(4), width_, body_h_);
        }
    }

    void hide_block(Block &b) {
        show(b.circle, false);  // its icon with it
        show(b.label, false);
        show(b.value, false);
    }
    void show_block(Block &b) {
        show(b.circle, true);
        show(b.label, true);
        show(b.value, true);
    }

    // A circle tinted in its colour with the icon in full colour while power flows there; grey while not.
    static void paint_circle(Block &b, uint32_t icon, theme::Role role, bool active) {
        show(b.circle, true);
        ui::set_fill(b.circle, Tone::NORMAL, active ? role : theme::TRACK);
        set_bg_opa(b.circle, active ? LV_OPA_20 : LV_OPA_60);
        ui::set_text(b.icon, ui::icon(icon));
        if (active) set_readable(b.icon, Tone::NORMAL, role);
        else ui::set_color(b.icon, theme::MUTED);
    }

    void paint_blocks(const Reading &r, bool has_pv, bool has_bat) {
        const bool importing = r.grid > FLOWING, exporting = r.grid < -FLOWING;
        const theme::Role grid_role = exporting ? EXPORT : IMPORT;
        Block &g = blocks_[GRID];
        show_block(g);
        paint_circle(g, ICON_GRID, grid_role, importing || exporting);
        ui::set_text(g.label, plugin_->text(exporting ? "export" : "import"));
        ui::set_color(g.label, theme::MUTED);
        ui::set_text(g.value, plugin_->power_text(r.grid));
        ui::set_color(g.value, theme::INK);

        const bool sunny = has_pv && r.pv > FLOWING;
        Block &s = blocks_[SUN];
        // The battery: green while it charges, amber while it gives; its state of charge beside its name.
        const bool charging = has_bat && r.bat < -FLOWING, discharging = has_bat && r.bat > FLOWING;
        const theme::Role bat_role = charging ? theme::MARK_GREEN : SOLAR;
        Block &b = blocks_[BATTERY];
        if (wide_) {
            if (has_pv) {
                show_block(s);
                paint_circle(s, ICON_SOLAR, SOLAR, sunny);
                ui::set_text(s.label, plugin_->text("solar"));
                ui::set_color(s.label, theme::MUTED);
                ui::set_text(s.value, plugin_->power_text(r.pv));
                ui::set_color(s.value, theme::INK);
            } else {
                hide_block(s);
            }
            if (has_bat) {
                show_block(b);
                paint_circle(b, ICON_BATTERY, bat_role, charging || discharging);
                ui::set_text(b.label, !std::isnan(r.soc)
                                          ? std::string(plugin_->text("battery")) + " " + std::to_string(percent(r.soc)) + " %"
                                          : std::string(plugin_->text("battery")));
                ui::set_color(b.label, theme::MUTED);
                ui::set_text(b.value, plugin_->power_text(r.bat));
                ui::set_color(b.value, theme::INK);
            } else {
                hide_block(b);
            }
            Block &h = blocks_[HOME];
            show_block(h);
            paint_circle(h, ICON_HOME, theme::INK, false);
            ui::set_color(h.icon, theme::INK);
            ui::set_text(h.label, plugin_->text("home"));
            ui::set_color(h.label, theme::MUTED);
            ui::set_text(h.value, plugin_->power_text(r.home));
            ui::set_color(h.value, theme::INK);
            // The sun and the battery flow to the house; the grid to the house (leftwards) or from it (rightwards).
            const bool flowing = charging || discharging;
            show(arrows_[0], sunny);
            show(arrows_[1], flowing);
            show(arrows_[2], importing || exporting);
            ui::set_text(arrows_[0], sunny ? ui::icon(ICON_RIGHT) : "");
            set_readable(arrows_[0], Tone::NORMAL, SOLAR);
            ui::set_text(arrows_[1], charging ? ui::icon(ICON_LEFT) : discharging ? ui::icon(ICON_RIGHT) : "");
            set_readable(arrows_[1], Tone::NORMAL, bat_role);
            ui::set_text(arrows_[2], importing ? ui::icon(ICON_LEFT) : exporting ? ui::icon(ICON_RIGHT) : "");
            set_readable(arrows_[2], Tone::NORMAL, grid_role);
        } else {
            // Narrow: "Solar 1,24 kW" and "Batterie 82 % · 350 W" in lines under the grid.
            show(s.label, has_pv);
            ui::set_text(s.label, has_pv ? std::string(plugin_->text("solar")) + " " + plugin_->power_text(r.pv) : "");
            if (sunny) set_readable(s.label, Tone::NORMAL, SOLAR);
            else ui::set_color(s.label, theme::MUTED);
            if (has_bat) {
                show(b.label, true);
                std::string line = plugin_->text("battery");
                if (!std::isnan(r.soc)) line += " " + tessera::format(plugin_->text("percent"), percent(r.soc));
                line += " · " + plugin_->power_text(r.bat);
                ui::set_text(b.label, line);
                if (charging || discharging) set_readable(b.label, Tone::NORMAL, bat_role);
                else ui::set_color(b.label, theme::MUTED);
            } else {
                show(b.label, false);
            }
        }
    }

    // The house's use split in what the sun covers (amber) and what comes from the grid (blue).
    void paint_share(const Reading &r, bool on) {
        show(share_track_, on);
        const int sun_w = on ? static_cast<int>(width_ * r.coverage / 100.0f + 0.5f) : 0;
        const int grid_w = on ? width_ - sun_w : 0;
        show(share_sun_, on && sun_w > 0);
        show(share_grid_, on && grid_w > 0 && r.grid > FLOWING);
        if (on) {
            place(share_sun_, 0, share_y_, std::max(1, sun_w), bar_h_);
            place(share_grid_, sun_w, share_y_, std::max(1, grid_w), bar_h_);
            ui::set_fill(share_sun_, Tone::NORMAL, SOLAR);
            ui::set_fill(share_grid_, Tone::NORMAL, IMPORT);
        }
        show(share_left_, on);
        show(share_right_, on);
        ui::set_text(share_left_, on ? tessera::format(plugin_->text("coverage"), percent(r.coverage)) : "");
        ui::set_text(share_right_,
                     on && r.grid > FLOWING ? tessera::format(plugin_->text("grid_share"), 100 - percent(r.coverage)) : "");
        ui::set_color(share_left_, theme::MUTED);
        ui::set_color(share_right_, theme::MUTED);
    }

    // What the panels made today against the forecast.
    void paint_forecast(const Reading &r, bool on) {
        show(fc_track_, on);
        show(fc_fill_, on && r.pv_today > 0);
        show(fc_label_, on);
        if (!on) {
            ui::set_text(fc_label_, "");
            return;
        }
        const float share = std::min(1.0f, r.pv_today / r.forecast);
        place(fc_fill_, 0, fc_y_, std::max(1, static_cast<int>(width_ * share + 0.5f)), bar_h_);
        ui::set_fill(fc_fill_, Tone::NORMAL, SOLAR);
        std::string label = tessera::fill(plugin_->text("forecast"), "made", plugin_->energy_text(r.pv_today));
        label = tessera::fill(label.c_str(), "of", plugin_->energy_text(r.forecast));
        ui::set_text(fc_label_, label + " · " + std::to_string(percent(100.0f * r.pv_today / r.forecast)) + " %");
        ui::set_color(fc_label_, theme::MUTED);
    }

    // "Today: solar 3,4 kWh · from the grid 5,2 kWh · battery 1,2 kWh"; only what is known.
    std::string day_text(const Reading &r) const {
        std::string out;
        auto add = [&](const char *key, float v) {
            if (std::isnan(v)) return;
            out += out.empty() ? std::string(plugin_->text("today")) + ": " : std::string(" · ");
            out += tessera::fill(plugin_->text(key), "v", plugin_->energy_text(v));
        };
        add("day_solar", r.pv_today);
        add("day_grid", r.grid_today);
        return out;
    }

    const Flux *plugin_;
    int width_ = 0, height_ = 0, label_h_ = 0, value_h_ = 0, body_h_ = 0, gap_ = 0, bar_h_ = 0, arrow_w_ = 0;
    int dia_ = 0, main_h_ = 0, row_h_ = 0, share_y_ = 0, fc_y_ = 0, shown_ = -1;
    Font value_font_ = Font::TITLE;
    bool today_ = true, wide_ = true, big_icon_ = false;
    Block blocks_[4];
    lv_obj_t *arrows_[3]{};
    lv_obj_t *share_track_{}, *share_sun_{}, *share_grid_{}, *share_left_{}, *share_right_{};
    lv_obj_t *day_{}, *fc_track_{}, *fc_fill_{}, *fc_label_{}, *note_{};
};

// ---- The tile --------------------------------------------------------------------------------------------------------

class FluxTile : public tessera::Tile {
 public:
    explicit FluxTile(const Flux *plugin) : plugin_(plugin), view_(plugin) {}

    void create(const tessera::TileContext &c) override {
        tile_ = c.tile;
        invert_ = c.options["invert"] | false;
        bat_invert_ = c.options["bat_invert"] | false;
        view_.create(c.parent, c.width, c.height, c.options["today"] | true, false);
    }
    void on_tick(uint32_t) override { view_.paint(plugin_->read(invert_, bat_invert_)); }
    void on_theme() override {
        view_.on_theme();
        on_tick(0);
    }
    void on_tap() override {
        plugin_->card_invert = invert_;
        plugin_->card_bat_invert = bat_invert_;
        tessera::open_card(plugin_->plugin_id(), "details", "", tile_);
    }

 private:
    const Flux *plugin_;
    FlowView view_;
    int tile_ = -1;
    bool invert_ = false, bat_invert_ = false;
};

// ---- The card --------------------------------------------------------------------------------------------------------

static constexpr uint32_t ICON_PANEL = 0xF0D9B;  // solar-panel
static constexpr uint32_t ICON_LEAF = 0xF032A;   // leaf
static constexpr uint32_t ICON_SUNNY = 0xF0599;  // weather-sunny
static constexpr uint32_t ICON_METER = 0xF1A57;  // meter-electric

static void set_border(lv_obj_t *obj, theme::Role role) {
    const lv_color_t c = ui::color(role);
    if (!lv_color_eq(lv_obj_get_style_border_color(obj, LV_PART_MAIN), c)) lv_obj_set_style_border_color(obj, c, 0);
}

// A tap on the tile: at the left the flow as in Home Assistant's energy card (the sun above, the battery, the grid and
// the house below, joined by lines with a dot where power flows), at the right every sensor in a row of its own.
class DetailsCard : public tessera::Card {
 public:
    explicit DetailsCard(const Flux *plugin) : plugin_(plugin) {}

    void open(const tessera::CardContext &c) override {
        width_ = c.width;
        height_ = c.height;
        invert_ = plugin_->card_invert;
        bat_invert_ = plugin_->card_bat_invert;
        const bool large = ui::large();
        line_w_ = std::max(2, ui::px(large ? 3 : 2));
        ring_w_ = std::max(2, ui::px(large ? 4 : 3));
        dot_ = ui::px(large ? 12 : 8);
        label_h_ = ui::line_height(Font::BODY);
        value_h_ = ui::line_height(Font::BODY_LARGE);

        // The diagram takes the left part, as wide as its height allows; the list the rest.
        const int gap = ui::px(large ? 32 : 16);
        diagram_w_ = std::min(width_ * 11 / 20, height_ * 3 / 2);
        list_x_ = diagram_w_ + gap;
        list_w_ = width_ - list_x_;
        dia_ = std::min({diagram_w_ * 10 / 32, (height_ - 2 * label_h_) * 10 / 22, ui::mm(30)});

        // Lines first, so the circles lie over their ends.
        for (lv_obj_t *&l : lines_) l = ui::block(c.parent, theme::TRACK);
        for (lv_obj_t *&d : dots_) {
            d = ui::block(c.parent, theme::TRACK);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        }
        for (Node &n : nodes_) {
            n.ring = ui::block(c.parent, theme::CARD);
            lv_obj_set_style_radius(n.ring, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(n.ring, ring_w_, 0);
            lv_obj_set_style_border_side(n.ring, LV_BORDER_SIDE_FULL, 0);
            n.icon = ui::label(n.ring, Font::ICON, theme::INK);
            lv_obj_set_style_text_align(n.icon, LV_TEXT_ALIGN_CENTER, 0);
            n.value = ui::label(n.ring, Font::BODY_LARGE, theme::INK);
            lv_obj_set_style_text_align(n.value, LV_TEXT_ALIGN_CENTER, 0);
            n.extra = ui::label(n.ring, Font::BODY, theme::MUTED);
            lv_obj_set_style_text_align(n.extra, LV_TEXT_ALIGN_CENTER, 0);
            n.name = ui::label(c.parent, Font::BODY, theme::MUTED);
            lv_obj_set_style_text_align(n.name, LV_TEXT_ALIGN_CENTER, 0);
        }
        layout_diagram();

        for (Row &r : rows_) {
            r.icon = ui::label(c.parent, Font::ICON_SMALL, theme::MUTED);
            lv_obj_set_style_text_align(r.icon, LV_TEXT_ALIGN_CENTER, 0);
            r.name = ui::label(c.parent, Font::BODY_LARGE, theme::INK_SOFT);
            r.value = ui::label(c.parent, Font::TITLE, theme::INK);
            lv_obj_set_style_text_align(r.value, LV_TEXT_ALIGN_RIGHT, 0);
            r.rule = ui::block(c.parent, theme::TRACK);
        }
        note_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
        place(note_, 0, (height_ - value_h_) / 2, width_, value_h_);
        on_tick(0);
    }

    void on_tick(uint32_t) override {
        const Reading r = plugin_->read(invert_, bat_invert_);
        ui::set_text(note_, r.have_grid ? "" : plugin_->text("choose"));
        paint_diagram(r);
        paint_list(r);
    }

    void on_theme() override { on_tick(0); }

 private:
    struct Node {
        lv_obj_t *ring{}, *icon{}, *value{}, *extra{}, *name{};
        int cx = 0, cy = 0;
    };
    struct Row {
        lv_obj_t *icon{}, *name{}, *value{}, *rule{};
    };
    enum { SUN, BATTERY, HOME, GRID };
    // The lines: the sun down to the battery, the grid and the house, the battery to the house, the grid to the house.
    enum { SUN_DOWN_B, SUN_TO_B, SUN_DOWN_G, SUN_TO_G, SUN_DOWN_H, SUN_TO_H, BAT_HOME, GRID_HOME, LINES };
    enum { DOT_SUN_B, DOT_SUN_G, DOT_SUN_H, DOT_BAT_HOME, DOT_GRID_HOME, DOTS };
    static constexpr int MAX_ROWS = 8;

    void layout_diagram() {
        const int r = dia_ / 2;
        const int bottom_cy = height_ - label_h_ - ui::px(6) - r;
        const int top_cy = label_h_ + ui::px(6) + r;
        nodes_[SUN].cx = diagram_w_ / 2, nodes_[SUN].cy = top_cy;
        nodes_[BATTERY].cx = r, nodes_[BATTERY].cy = bottom_cy;
        nodes_[HOME].cx = diagram_w_ / 2, nodes_[HOME].cy = bottom_cy;
        nodes_[GRID].cx = diagram_w_ - r, nodes_[GRID].cy = bottom_cy;
        // What fits inside a ring: the value in the larger font where "8.88 kW" fits its width, the large icon and
        // the day's energy under the value only where the three fit its height.
        const int inner = dia_ - 2 * ring_w_ - ui::px(6);
        const Font value_font = ui::text_width("8.88 kW", Font::BODY_LARGE) <= inner ? Font::BODY_LARGE : Font::BODY;
        const int vh = ui::line_height(value_font);
        const Font icon_font = ui::line_height(Font::ICON) + vh + label_h_ <= inner ? Font::ICON : Font::ICON_SMALL;
        const int ih = ui::line_height(icon_font);
        show_extra_ = ih + vh + label_h_ <= inner;
        const int text_block = vh + (show_extra_ ? label_h_ : 0);
        for (int i = 0; i < 4; ++i) {
            Node &n = nodes_[i];
            place(n.ring, n.cx - r, n.cy - r, dia_, dia_);
            // Icon, value and SOC inside the ring, as its children: one part, the three centred around its middle.
            const int start = -(ih + text_block) / 2;
            ui::set_font(n.icon, icon_font);
            ui::set_font(n.value, value_font);
            inside(n.ring, n.icon, dia_ - 2 * ring_w_, ih, start + ih / 2);
            inside(n.ring, n.value, dia_ - 2 * ring_w_, vh, start + ih + vh / 2);
            inside(n.ring, n.extra, dia_ - 2 * ring_w_, label_h_, start + ih + vh + label_h_ / 2);
            show(n.extra, show_extra_);
            // The sun's name above it, the others' below.
            const int ny = i == SUN ? n.cy - r - label_h_ - ui::px(4) : n.cy + r + ui::px(4);
            // The name a little wider than its circle, but inside the card at the edges.
            const int name_w = dia_ + ui::px(40);
            place(n.name, std::max(0, std::min(n.cx - name_w / 2, width_ - name_w)), std::max(0, ny), name_w, label_h_);
        }
        // Lines as in Home Assistant's card: from the bottom of the sun one bends to the battery, one to the grid and
        // one to the house, a little above the middle of the lower circles; the battery's and the grid's lines to the
        // house a little below it.
        const int off = r / 4, lw = line_w_;
        const Node &sun = nodes_[SUN], &bat = nodes_[BATTERY], &home = nodes_[HOME], &grid = nodes_[GRID];
        upper_y_ = grid.cy - r / 4;
        lower_y_ = grid.cy + r / 4;
        const int sun_bottom = sun.cy + r - ring_w_;
        place(lines_[SUN_DOWN_B], sun.cx - off, sun_bottom, lw, upper_y_ - sun_bottom + lw);
        place(lines_[SUN_TO_B], bat.cx + r - ring_w_, upper_y_, sun.cx - off - (bat.cx + r - ring_w_) + lw, lw);
        place(lines_[SUN_DOWN_G], sun.cx + off, sun_bottom, lw, upper_y_ - sun_bottom + lw);
        place(lines_[SUN_TO_G], sun.cx + off, upper_y_, grid.cx + r - ring_w_ - sun.cx - off + lw, lw);
        place(lines_[SUN_DOWN_H], sun.cx, sun_bottom, lw, upper_y_ - sun_bottom + lw);
        place(lines_[SUN_TO_H], sun.cx, upper_y_, home.cx - r + ring_w_ - sun.cx + lw, lw);
        place(lines_[BAT_HOME], bat.cx + r - ring_w_, lower_y_, home.cx - bat.cx - 2 * (r - ring_w_), lw);
        place(lines_[GRID_HOME], grid.cx + r - ring_w_, lower_y_, home.cx - grid.cx - 2 * (r - ring_w_), lw);
        const int d = dot_;
        place(dots_[DOT_SUN_B], (bat.cx + r + sun.cx - off) / 2 - d / 2, upper_y_ + lw / 2 - d / 2, d, d);
        place(dots_[DOT_SUN_G], (sun.cx + off + grid.cx - r) / 2 - d / 2, upper_y_ + lw / 2 - d / 2, d, d);
        place(dots_[DOT_SUN_H], (sun.cx + home.cx - r) / 2 - d / 2, upper_y_ + lw / 2 - d / 2, d, d);
        place(dots_[DOT_BAT_HOME], (bat.cx + r + home.cx - r) / 2 - d / 2, lower_y_ + lw / 2 - d / 2, d, d);
        place(dots_[DOT_GRID_HOME], (grid.cx + r + home.cx - r) / 2 - d / 2, lower_y_ + lw / 2 - d / 2, d, d);
    }

    void paint_node(Node &n, uint32_t icon, theme::Role role, bool active, const std::string &value,
                    const std::string &extra, const char *name) {
        set_border(n.ring, active ? role : theme::TRACK);
        ui::set_fill(n.ring, Tone::NORMAL, theme::CARD);
        ui::set_text(n.icon, ui::icon(icon));
        if (active) set_readable(n.icon, Tone::NORMAL, role);
        else ui::set_color(n.icon, theme::MUTED);
        ui::set_text(n.value, value);
        ui::set_color(n.value, theme::INK);
        ui::set_text(n.extra, show_extra_ ? extra : std::string());
        ui::set_color(n.extra, theme::MUTED);
        ui::set_text(n.name, name);
        ui::set_color(n.name, theme::MUTED);
    }

    void paint_line(int a, int b, int dot, theme::Role role, bool active) {
        for (int i : {a, b})
            if (i >= 0) ui::set_fill(lines_[i], Tone::NORMAL, active ? role : theme::TRACK);
        show(dots_[dot], active);
        ui::set_fill(dots_[dot], Tone::NORMAL, role);
    }

    void paint_diagram(const Reading &r) {
        const bool on = r.have_grid;
        for (Node &n : nodes_) {
            show(n.ring, on);
            if (!on) ui::set_text(n.icon, ""), ui::set_text(n.value, ""), ui::set_text(n.extra, ""), ui::set_text(n.name, "");
        }
        for (lv_obj_t *l : lines_) show(l, on);
        if (!on) {
            for (lv_obj_t *d : dots_) show(d, false);
            return;
        }
        const bool has_pv = !std::isnan(r.pv);
        const bool has_bat = r.have_bat;
        const float pv = has_pv ? r.pv : 0.0f;
        const bool importing = r.grid > FLOWING, exporting = r.grid < -FLOWING;
        const bool charging = has_bat && r.bat < -FLOWING, discharging = has_bat && r.bat > FLOWING;
        const theme::Role bat_role = charging ? theme::MARK_GREEN : SOLAR;
        const float sun_home = std::min(pv, r.home);
        paint_node(nodes_[SUN], ICON_SOLAR, SOLAR, pv > FLOWING, has_pv ? plugin_->power_text(pv) : "–",
                   std::isnan(r.pv_today) ? std::string() : plugin_->energy_text(r.pv_today), plugin_->text("solar"));
        // The screen's fonts have no arrows: the colour and the name say which way the power goes.
        paint_node(nodes_[BATTERY], ICON_BATTERY, bat_role, charging || discharging,
                   has_bat ? plugin_->power_text(r.bat) : "–",
                   !std::isnan(r.soc) ? tessera::format(plugin_->text("percent"), percent(r.soc)) : std::string(),
                   plugin_->text("battery"));
        paint_node(nodes_[GRID], ICON_GRID, exporting ? EXPORT : IMPORT, importing || exporting,
                   plugin_->power_text(r.grid),
                   std::isnan(r.grid_today) ? std::string() : plugin_->energy_text(r.grid_today),
                   plugin_->text(exporting ? "export" : importing ? "import" : "grid"));
        paint_node(nodes_[HOME], ICON_HOME, theme::ACCENT, r.home > FLOWING, plugin_->power_text(r.home),
                   std::isnan(r.coverage) ? std::string() : tessera::format(plugin_->text("percent"), percent(r.coverage)),
                   plugin_->text("home"));
        paint_line(SUN_DOWN_B, SUN_TO_B, DOT_SUN_B, SOLAR, has_pv && charging);
        paint_line(SUN_DOWN_G, SUN_TO_G, DOT_SUN_G, EXPORT, has_pv && exporting);
        paint_line(SUN_DOWN_H, SUN_TO_H, DOT_SUN_H, SOLAR, sun_home > FLOWING);
        paint_line(BAT_HOME, -1, DOT_BAT_HOME, bat_role, charging || discharging);
        paint_line(GRID_HOME, -1, DOT_GRID_HOME, IMPORT, importing);
        // Without a solar sensor, no lines from the sun.
        for (int i : {SUN_DOWN_B, SUN_TO_B, SUN_DOWN_G, SUN_TO_G, SUN_DOWN_H, SUN_TO_H}) show(lines_[i], has_pv);
        show(nodes_[SUN].ring, has_pv);
        if (!has_pv) ui::set_text(nodes_[SUN].icon, ""), ui::set_text(nodes_[SUN].value, ""),
            ui::set_text(nodes_[SUN].extra, ""), ui::set_text(nodes_[SUN].name, "");
        // Without a battery sensor, no battery and no battery lines.
        if (!has_bat) {
            show(nodes_[BATTERY].ring, false);
            ui::set_text(nodes_[BATTERY].icon, ""), ui::set_text(nodes_[BATTERY].value, ""),
                ui::set_text(nodes_[BATTERY].extra, ""), ui::set_text(nodes_[BATTERY].name, "");
            for (int i : {SUN_DOWN_B, SUN_TO_B, BAT_HOME}) show(lines_[i], false);
            show(dots_[DOT_SUN_B], false);
            show(dots_[DOT_BAT_HOME], false);
        }
    }

    // Every sensor in a row: its icon, its name and its value; only those chosen.
    void paint_list(const Reading &r) {
        struct Item {
            uint32_t icon;
            theme::Role role;
            std::string name, value;
        };
        Item items[MAX_ROWS];
        int n = 0;
        if (r.have_grid) {
            const bool exporting = r.grid < -FLOWING;
            items[n++] = {ICON_GRID, exporting ? EXPORT : IMPORT, plugin_->text(exporting ? "export" : "import"),
                          plugin_->power_text(r.grid)};
            if (!std::isnan(r.pv)) items[n++] = {ICON_SOLAR, SOLAR, plugin_->text("row_pv"), plugin_->power_text(r.pv)};
            if (r.have_bat) {
                const std::string soc = !std::isnan(r.soc)
                                            ? tessera::fill(plugin_->text("battery_soc"), "n", std::to_string(percent(r.soc)))
                                            : std::string();
                items[n++] = {ICON_BATTERY, r.bat < -FLOWING ? theme::MARK_GREEN : SOLAR, plugin_->text("row_battery"),
                              plugin_->power_text(r.bat) + (soc.empty() ? "" : " · " + soc)};
            }
            items[n++] = {ICON_HOME, theme::ACCENT, plugin_->text("row_home"), plugin_->power_text(r.home)};
            if (!std::isnan(r.coverage))
                items[n++] = {ICON_LEAF, theme::MARK_GREEN, plugin_->text("row_coverage"),
                              tessera::format(plugin_->text("percent"), percent(r.coverage))};
            if (!std::isnan(r.pv_today))
                items[n++] = {ICON_PANEL, SOLAR, plugin_->text("row_pv_today"), plugin_->energy_text(r.pv_today)};
            if (!std::isnan(r.forecast)) {
                std::string v = plugin_->energy_text(r.forecast);
                if (!std::isnan(r.pv_today) && r.forecast > 0)
                    v += " · " + tessera::format(plugin_->text("percent"), percent(100.0f * r.pv_today / r.forecast));
                items[n++] = {ICON_SUNNY, SOLAR, plugin_->text("row_forecast"), v};
            }
            if (!std::isnan(r.grid_today))
                items[n++] = {ICON_METER, IMPORT, plugin_->text("row_grid_today"), plugin_->energy_text(r.grid_today)};
        }
        // The rows share the height, at most a comfortable row each, in the middle.
        const int ih = ui::line_height(Font::ICON_SMALL), th = ui::line_height(Font::TITLE);
        // On a narrow list the names in the smaller font; each value as wide as it is, the name taking the rest.
        const Font name_font = list_w_ >= ui::mm(70) ? Font::BODY_LARGE : Font::BODY;
        const int name_h = ui::line_height(name_font);
        const int row_h = n ? std::min(height_ / n, std::max(ih, th) + ui::px(ui::large() ? 28 : 14)) : 0;
        const int top = (height_ - n * row_h) / 2;
        if (n != rows_shown_) {
            rows_shown_ = n;
            for (int i = 0; i < MAX_ROWS; ++i) {
                Row &row = rows_[i];
                const int y = top + i * row_h;
                ui::set_font(row.name, name_font);
                place(row.icon, list_x_, y + (row_h - ih) / 2, ih, ih);
                place(row.rule, list_x_, y + row_h - 1, list_w_, 1);
            }
        }
        for (int i = 0; i < MAX_ROWS; ++i) {
            Row &row = rows_[i];
            const bool on = i < n;
            ui::set_text(row.icon, on ? ui::icon(items[i].icon) : "");
            ui::set_text(row.name, on ? items[i].name : "");
            ui::set_text(row.value, on ? items[i].value : "");
            if (on) {
                const int y = top + i * row_h;
                const int name_x = list_x_ + ih + ui::px(10);
                const int value_w = std::min(list_w_ * 3 / 5, ui::text_width(items[i].value.c_str(), Font::TITLE) + ui::px(4));
                place(row.name, name_x, y + (row_h - name_h) / 2, list_x_ + list_w_ - value_w - ui::px(8) - name_x, name_h);
                place(row.value, list_x_ + list_w_ - value_w, y + (row_h - th) / 2, value_w, th);
            }
            if (on) {
                set_readable(row.icon, Tone::NORMAL, items[i].role);
                ui::set_color(row.name, theme::INK_SOFT);
                ui::set_color(row.value, theme::INK);
            }
            show(row.rule, on && i < n - 1);
            ui::set_fill(row.rule, Tone::NORMAL, theme::TRACK);
        }
    }

    const Flux *plugin_;
    int width_ = 0, height_ = 0, diagram_w_ = 0, list_x_ = 0, list_w_ = 0, dia_ = 0, line_w_ = 0, ring_w_ = 0;
    int dot_ = 0, label_h_ = 0, value_h_ = 0, upper_y_ = 0, lower_y_ = 0, rows_shown_ = -1;
    bool show_extra_ = true;
    bool invert_ = false, bat_invert_ = false;
    Node nodes_[4];
    lv_obj_t *lines_[LINES]{}, *dots_[DOTS]{};
    Row rows_[MAX_ROWS];
    lv_obj_t *note_{};
};

void Flux::setup() {
    add_tile("flux", [this]() { return new FluxTile(this); });
    add_card("details", [this]() { return new DetailsCard(this); }, true);
}

}  // namespace esphome::flux
