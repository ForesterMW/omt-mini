#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace ui {

using gfx::theme;

// ---- layout helpers ----------------------------------------------------
D2D1_RECT_F rect(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}
D2D1_RECT_F inset(const D2D1_RECT_F& r, float dx, float dy) {
    return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}
D2D1_RECT_F row(const D2D1_RECT_F& area, float y, float height) {
    return D2D1::RectF(area.left, y, area.right, y + height);
}
bool contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---- frame -------------------------------------------------------------
void Ctx::begin(ID2D1DeviceContext* dc, float width, float height, Input* input) {
    dc_     = dc;
    input_  = input;
    width_  = width;
    height_ = height;
    needs_redraw_ = false;
    popup_pending_ = false;
    hot_ = kNoId;

    claimed_.clear();

    const double now = static_cast<double>(util::now_ms());
    dt_ = last_time_ > 0.0
        ? std::min(0.1f, static_cast<float>((now - last_time_) / 1000.0))
        : 0.016f;
    last_time_ = now;

    if (!brush_ && dc_)
        dc_->CreateSolidColorBrush(theme().text, brush_.put());
}

void Ctx::end() {
    // A popup whose owner was not drawn this frame cannot be dismissed by the
    // code below, because that only runs for a popup that is still being laid
    // out. Left alone it would keep owning every click in the window and no
    // button anywhere would work again. Switching tab with a list open, or
    // reopening the window on a different tab, both land here.
    if (open_dropdown_ != kNoId && !popup_pending_) {
        open_dropdown_ = kNoId;
        needs_redraw_ = true;
    }

    // Popup lists draw last so they sit above the rest of the window.
    if (popup_pending_) {
        const auto& p = popup_;
        const float item_h = 26.0f;
        const float list_h = std::min(260.0f, item_h * static_cast<float>(p.items.size()) + 8.0f);

        float top = p.anchor.bottom + 4.0f;
        if (top + list_h > height_ - 4.0f)
            top = std::max(4.0f, p.anchor.top - list_h - 4.0f);

        const D2D1_RECT_F list = D2D1::RectF(p.anchor.left, top, p.anchor.right, top + list_h);

        // Shadow, then the list surface.
        fill_rect(D2D1::RectF(list.left + 2, list.top + 3, list.right + 2, list.bottom + 3),
                  gfx::rgb(0x000000, 0.35f), metric::kRadius);
        fill_rect(list, theme().panel_hi, metric::kRadius);
        stroke_rect(list, theme().border, 1.0f, metric::kRadius);

        push_clip(inset(list, 1.0f, 4.0f));
        float y = list.top + 4.0f;
        for (size_t i = 0; i < p.items.size(); ++i) {
            const D2D1_RECT_F ir = D2D1::RectF(list.left + 4, y, list.right - 4, y + item_h);
            const bool over = hovered(ir);
            const bool sel  = static_cast<int>(i) == p.selected;

            if (over)      fill_rect(ir, theme().accent, 4.0f);
            else if (sel)  fill_rect(ir, theme().panel_sel, 4.0f);

            text(inset(ir, 8.0f, 0.0f), p.items[i], Font::Body,
                 over ? theme().on_accent : theme().text);

            if (sel && !over)
                glyph(Glyph::Check, D2D1::RectF(ir.right - 20, ir.top + 8, ir.right - 8, ir.bottom - 8),
                      theme().accent, 1.6f);

            if (over && input_->mouse_pressed) {
                dropdown_changed_ = p.id;
                dropdown_value_   = static_cast<int>(i);
                open_dropdown_    = kNoId;
                needs_redraw_     = true;
            }
            y += item_h;
        }
        pop_clip();

        // A click anywhere else dismisses the list.
        if (input_->mouse_pressed && !contains(list, input_->mouse_x, input_->mouse_y) &&
            !contains(p.anchor, input_->mouse_x, input_->mouse_y)) {
            open_dropdown_ = kNoId;
            needs_redraw_ = true;
        }
    }

    if (input_->mouse_released) active_ = kNoId;
    if (!clip_stack_.empty()) {
        // Defensive: a widget that returned early should not leak a clip.
        while (!clip_stack_.empty()) pop_clip();
    }
}

// ---- primitives --------------------------------------------------------
ID2D1SolidColorBrush* Ctx::brush(const D2D1_COLOR_F& c) {
    if (!brush_) return nullptr;
    brush_->SetColor(c);
    return brush_.get();
}

D2D1_COLOR_F Ctx::mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) const {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

void Ctx::fill_rect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius) {
    if (!dc_) return;
    auto* b = brush(c);
    if (radius > 0.5f) {
        D2D1_ROUNDED_RECT rr{ r, radius, radius };
        dc_->FillRoundedRectangle(&rr, b);
    } else {
        dc_->FillRectangle(&r, b);
    }
}

void Ctx::stroke_rect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float thickness, float radius) {
    if (!dc_) return;
    auto* b = brush(c);
    // Half pixel offset keeps a one pixel stroke crisp.
    const D2D1_RECT_F s = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
    if (radius > 0.5f) {
        D2D1_ROUNDED_RECT rr{ s, radius, radius };
        dc_->DrawRoundedRectangle(&rr, b, thickness);
    } else {
        dc_->DrawRectangle(&s, b, thickness);
    }
}

void Ctx::line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& c, float thickness) {
    if (!dc_) return;
    dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), thickness);
}

void Ctx::text(const D2D1_RECT_F& r, const std::wstring& s, Font f,
               const D2D1_COLOR_F& c, Align a, bool vcenter) {
    if (!dc_ || s.empty()) return;
    auto* fmt = gfx::text_format(f);
    if (!fmt) return;

    fmt->SetTextAlignment(a == Align::Left   ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                             : DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                       : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    dc_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, &r, brush(c),
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Ctx::text_wrapped(const D2D1_RECT_F& r, const std::wstring& s, Font f, const D2D1_COLOR_F& c) {
    if (!dc_ || s.empty()) return;
    auto* fmt = gfx::text_format(f);
    if (!fmt) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    dc_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, &r, brush(c),
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

float Ctx::text_width(const std::wstring& s, Font f) {
    if (s.empty()) return 0.0f;
    auto* fmt = gfx::text_format(f);
    if (!fmt || !gfx::Device::dwrite()) return 0.0f;

    gfx::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(gfx::Device::dwrite()->CreateTextLayout(
            s.c_str(), static_cast<UINT32>(s.size()), fmt, 4096.0f, 100.0f, layout.put())))
        return 0.0f;

    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.widthIncludingTrailingWhitespace;
}

void Ctx::push_clip(const D2D1_RECT_F& r) {
    if (!dc_) return;
    dc_->PushAxisAlignedClip(&r, D2D1_ANTIALIAS_MODE_ALIASED);
    clip_stack_.push_back(r);
}

void Ctx::pop_clip() {
    if (!dc_ || clip_stack_.empty()) return;
    dc_->PopAxisAlignedClip();
    clip_stack_.pop_back();
}

void Ctx::glyph(Glyph g, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float thickness) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float s  = std::min(r.right - r.left, r.bottom - r.top) * 0.5f;

    switch (g) {
        case Glyph::ChevronDown:
            line(cx - s * 0.6f, cy - s * 0.25f, cx, cy + s * 0.35f, c, thickness);
            line(cx, cy + s * 0.35f, cx + s * 0.6f, cy - s * 0.25f, c, thickness);
            break;
        case Glyph::ChevronRight:
            line(cx - s * 0.25f, cy - s * 0.6f, cx + s * 0.35f, cy, c, thickness);
            line(cx + s * 0.35f, cy, cx - s * 0.25f, cy + s * 0.6f, c, thickness);
            break;
        case Glyph::Close:
            line(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f, c, thickness);
            line(cx + s * 0.5f, cy - s * 0.5f, cx - s * 0.5f, cy + s * 0.5f, c, thickness);
            break;
        case Glyph::Check:
            line(cx - s * 0.55f, cy, cx - s * 0.12f, cy + s * 0.45f, c, thickness);
            line(cx - s * 0.12f, cy + s * 0.45f, cx + s * 0.6f, cy - s * 0.45f, c, thickness);
            break;
        case Glyph::Dot: {
            D2D1_ELLIPSE e{ D2D1::Point2F(cx, cy), s * 0.35f, s * 0.35f };
            if (dc_) dc_->FillEllipse(&e, brush(c));
            break;
        }
        case Glyph::Plus:
            line(cx - s * 0.5f, cy, cx + s * 0.5f, cy, c, thickness);
            line(cx, cy - s * 0.5f, cx, cy + s * 0.5f, c, thickness);
            break;
        case Glyph::Minus:
            line(cx - s * 0.5f, cy, cx + s * 0.5f, cy, c, thickness);
            break;
        case Glyph::Gear: {
            D2D1_ELLIPSE outer{ D2D1::Point2F(cx, cy), s * 0.55f, s * 0.55f };
            D2D1_ELLIPSE inner{ D2D1::Point2F(cx, cy), s * 0.22f, s * 0.22f };
            if (dc_) {
                dc_->DrawEllipse(&outer, brush(c), thickness);
                dc_->DrawEllipse(&inner, brush(c), thickness);
            }
            break;
        }
        case Glyph::Pop:
            // "open in new window" arrow
            line(cx - s * 0.45f, cy + s * 0.45f, cx + s * 0.35f, cy - s * 0.35f, c, thickness);
            line(cx - s * 0.05f, cy - s * 0.45f, cx + s * 0.45f, cy - s * 0.45f, c, thickness);
            line(cx + s * 0.45f, cy - s * 0.45f, cx + s * 0.45f, cy + s * 0.05f, c, thickness);
            break;
    }
}

// ---- interaction -------------------------------------------------------
bool Ctx::hovered(const D2D1_RECT_F& r) const {
    if (!input_) return false;
    // A widget under an open popup must not react.
    if (open_dropdown_ != kNoId && popup_pending_) {
        // The popup itself is drawn during end(), where this guard is skipped.
    }
    return contains(r, input_->mouse_x, input_->mouse_y);
}

void Ctx::claim(Id id) {
    if (id == kNoId) return;
    for (Id other : claimed_) {
        if (other != id) continue;
        // Logged rather than asserted: a duplicate is a bug worth finding, but
        // not worth taking the application down over in front of an audience.
        static Id last_reported = kNoId;
        if (last_reported != id) {
            last_reported = id;
            util::logf("ui: widget id %u is used by more than one control in the "
                       "same frame; clicks on it will be unreliable",
                       static_cast<unsigned>(id));
        }
        return;
    }
    claimed_.push_back(id);
}

bool Ctx::consume_click(Id id, const D2D1_RECT_F& r, bool enabled) {
    if (!enabled || !input_) return false;
    claim(id);
    // While a dropdown list is open it owns all clicks.
    if (open_dropdown_ != kNoId && open_dropdown_ != id) return false;

    const bool over = hovered(r);
    if (over) hot_ = id;

    if (over && input_->mouse_pressed) {
        active_ = id;
        focus_  = id;
        needs_redraw_ = true;
    }
    if (active_ == id && input_->mouse_released) {
        active_ = kNoId;
        needs_redraw_ = true;
        return over;
    }
    return false;
}

float Ctx::animate(Id id, float target, float speed) {
    float& v = anim_[id];
    const float delta = target - v;
    if (std::fabs(delta) < 0.004f) {
        v = target;
        return v;
    }
    v += delta * std::min(1.0f, speed * dt_);
    needs_redraw_ = true;
    return v;
}

// ---- widgets -----------------------------------------------------------
bool Ctx::button(Id id, const D2D1_RECT_F& r, const std::wstring& label,
                 ButtonStyle style, bool enabled) {
    const bool clicked = consume_click(id, r, enabled);
    const bool over    = enabled && hovered(r) && (open_dropdown_ == kNoId);
    const bool down    = enabled && active_ == id && input_ && input_->mouse_down;

    const float t = animate(id, over ? 1.0f : 0.0f);

    D2D1_COLOR_F fill, fg, edge = theme().border;
    switch (style) {
        case ButtonStyle::Primary:
            fill = mix(theme().accent, theme().accent_hi, t);
            fg   = theme().on_accent;
            edge = fill;
            break;
        case ButtonStyle::Danger:
            fill = mix(theme().danger, gfx::rgb(0xF87171), t);
            fg   = theme().on_accent;
            edge = fill;
            break;
        case ButtonStyle::Ghost:
            fill = mix(gfx::rgb(0x000000, 0.0f), theme().panel_hi, t);
            fg   = theme().text;
            edge = gfx::rgb(0x000000, 0.0f);
            break;
        case ButtonStyle::Toolbar:
            fill = mix(gfx::rgb(0x000000, 0.0f), theme().panel_hi, t);
            fg   = theme().text_dim;
            edge = gfx::rgb(0x000000, 0.0f);
            break;
        default:
            fill = mix(theme().panel, theme().panel_hi, t);
            fg   = theme().text;
            break;
    }

    if (!enabled) {
        fill = theme().panel;
        fg   = theme().text_dim;
    }
    if (down) fill = mix(fill, gfx::rgb(0x000000), 0.18f);

    fill_rect(r, fill, metric::kRadius);
    if (edge.a > 0.01f) stroke_rect(r, edge, 1.0f, metric::kRadius);
    text(r, label, Font::Body, fg, Align::Center);
    return clicked;
}

bool Ctx::icon_button(Id id, const D2D1_RECT_F& r, Glyph g, const std::wstring&) {
    const bool clicked = consume_click(id, r);
    const bool over    = hovered(r) && (open_dropdown_ == kNoId);
    const float t      = animate(id, over ? 1.0f : 0.0f);

    if (t > 0.01f) fill_rect(r, mix(gfx::rgb(0x000000, 0.0f), theme().panel_hi, t), 5.0f);
    glyph(g, inset(r, 6.0f, 6.0f), mix(theme().text_dim, theme().text, t), 1.6f);
    return clicked;
}

void Ctx::lamp(const D2D1_RECT_F& r, const std::wstring& label, bool lit,
               const D2D1_COLOR_F& colour) {
    D2D1_COLOR_F fill = colour;
    D2D1_COLOR_F edge = colour;
    D2D1_COLOR_F fg;

    if (lit) {
        fg = theme().on_accent;
    } else {
        // Unlit reads as off rather than as disabled: the outline stays in the
        // lamp's own colour so it is obvious which one would light.
        fill.a = 0.10f;
        edge.a = 0.35f;
        fg = colour;
        fg.a = 0.55f;
    }

    fill_rect(r, fill, metric::kRadius);
    stroke_rect(r, edge, 1.0f, metric::kRadius);
    text(r, label, Font::BodyBold, fg, Align::Center);
}

bool Ctx::clicked_area(Id id, const D2D1_RECT_F& r) {
    return consume_click(id, r);
}

bool Ctx::checkbox(Id id, const D2D1_RECT_F& r, bool* value, const std::wstring& label) {
    const bool clicked = consume_click(id, r);
    if (clicked && value) *value = !*value;

    const bool over = hovered(r) && (open_dropdown_ == kNoId);
    const float t   = animate(id, over ? 1.0f : 0.0f);
    const bool on   = value && *value;

    const float box = 18.0f;
    const D2D1_RECT_F b = D2D1::RectF(r.left, (r.top + r.bottom) * 0.5f - box * 0.5f,
                                      r.left + box, (r.top + r.bottom) * 0.5f + box * 0.5f);

    fill_rect(b, on ? theme().accent : mix(theme().panel, theme().panel_hi, t), 4.0f);
    if (!on) stroke_rect(b, mix(theme().border, theme().text_dim, t), 1.0f, 4.0f);
    if (on)  glyph(Glyph::Check, inset(b, 3.5f, 3.5f), theme().on_accent, 1.8f);

    text(D2D1::RectF(b.right + 10.0f, r.top, r.right, r.bottom), label, Font::Body, theme().text);
    return clicked;
}

bool Ctx::toggle(Id id, const D2D1_RECT_F& r, bool* value, const std::wstring& label) {
    const bool clicked = consume_click(id, r);
    if (clicked && value) *value = !*value;

    const bool on = value && *value;
    const float t = animate(id ^ 0x5A5A, on ? 1.0f : 0.0f, 18.0f);

    const float tw = 38.0f, th = 20.0f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const D2D1_RECT_F track = D2D1::RectF(r.right - tw, cy - th * 0.5f, r.right, cy + th * 0.5f);

    fill_rect(track, mix(theme().panel_hi, theme().accent, t), th * 0.5f);
    if (t < 0.5f) stroke_rect(track, theme().border, 1.0f, th * 0.5f);

    const float knob_r = th * 0.5f - 3.0f;
    const float kx = track.left + knob_r + 3.0f + t * (tw - 2.0f * (knob_r + 3.0f));
    D2D1_ELLIPSE knob{ D2D1::Point2F(kx, cy), knob_r, knob_r };
    if (dc_) dc_->FillEllipse(&knob, brush(mix(theme().text_dim, theme().on_accent, t)));

    if (!label.empty())
        text(D2D1::RectF(r.left, r.top, track.left - 10.0f, r.bottom), label, Font::Body, theme().text);
    return clicked;
}

bool Ctx::slider(Id id, const D2D1_RECT_F& r, float* value, float min_v, float max_v) {
    if (!value || max_v <= min_v) return false;

    const float cy = (r.top + r.bottom) * 0.5f;
    const float track_h = 4.0f;
    const D2D1_RECT_F track = D2D1::RectF(r.left, cy - track_h * 0.5f, r.right, cy + track_h * 0.5f);

    consume_click(id, r);

    bool changed = false;
    if (active_ == id && input_ && input_->mouse_down) {
        const float t = std::clamp((input_->mouse_x - r.left) / std::max(1.0f, r.right - r.left),
                                   0.0f, 1.0f);
        const float nv = min_v + t * (max_v - min_v);
        if (std::fabs(nv - *value) > 0.0001f) { *value = nv; changed = true; needs_redraw_ = true; }
    }

    const float frac = std::clamp((*value - min_v) / (max_v - min_v), 0.0f, 1.0f);
    const float kx   = r.left + frac * (r.right - r.left);

    fill_rect(track, theme().panel_hi, track_h * 0.5f);
    fill_rect(D2D1::RectF(track.left, track.top, kx, track.bottom), theme().accent, track_h * 0.5f);

    const bool over = hovered(r);
    const float t   = animate(id ^ 0x3C3C, (over || active_ == id) ? 1.0f : 0.0f);
    const float kr  = 6.0f + t * 1.5f;
    D2D1_ELLIPSE knob{ D2D1::Point2F(kx, cy), kr, kr };
    if (dc_) {
        dc_->FillEllipse(&knob, brush(theme().on_accent));
        D2D1_ELLIPSE ring{ D2D1::Point2F(kx, cy), kr, kr };
        dc_->DrawEllipse(&ring, brush(theme().accent), 1.5f);
    }
    return changed;
}

bool Ctx::text_field(Id id, const D2D1_RECT_F& r, std::wstring* value,
                     const std::wstring& placeholder, bool numeric) {
    if (!value) return false;
    consume_click(id, r);

    const bool focused = focus_ == id;
    const bool over    = hovered(r) && (open_dropdown_ == kNoId);
    const float t      = animate(id, focused ? 1.0f : (over ? 0.45f : 0.0f));

    fill_rect(r, theme().panel, metric::kRadius);
    stroke_rect(r, mix(theme().border, theme().accent, t), focused ? 1.6f : 1.0f, metric::kRadius);

    bool changed = false;
    if (focused && input_) {
        for (wchar_t ch : input_->typed) {
            if (ch == L'\b') {
                if (!value->empty()) { value->pop_back(); changed = true; }
            } else if (ch >= 32 && ch != 127) {
                if (numeric && !std::iswdigit(ch) && ch != L'-') continue;
                if (value->size() < 512) { value->push_back(ch); changed = true; }
            }
        }
        if (changed) needs_redraw_ = true;
    }

    const D2D1_RECT_F tr = inset(r, 10.0f, 0.0f);
    push_clip(inset(r, 2.0f, 1.0f));
    if (value->empty() && !focused) {
        text(tr, placeholder, Font::Body, theme().text_dim);
    } else {
        text(tr, *value, Font::Body, theme().text);
        if (focused) {
            // Blinking caret, parked at the end of the text.
            const float w = text_width(*value, Font::Body);
            const bool  on = ((util::now_ms() / 500) % 2) == 0;
            if (on)
                line(tr.left + w + 1.5f, r.top + 8.0f, tr.left + w + 1.5f, r.bottom - 8.0f,
                     theme().text, 1.4f);
            needs_redraw_ = true;   // keep the caret animating
        }
    }
    pop_clip();
    return changed;
}

bool Ctx::dropdown(Id id, const D2D1_RECT_F& r, const std::vector<std::wstring>& items, int* index) {
    // Apply a selection that end() committed on the previous frame, before the
    // control is drawn, so it shows the new value on this frame rather than the
    // next one. The caller's index variable is alive again at this point,
    // which is the whole reason the selection travels as a value.
    bool changed = false;
    if (dropdown_changed_ == id) {
        dropdown_changed_ = kNoId;
        if (index && dropdown_value_ >= 0 &&
            dropdown_value_ < static_cast<int>(items.size())) {
            *index = dropdown_value_;
            changed = true;
        }
        dropdown_value_ = -1;
    }

    const bool is_open = open_dropdown_ == id;

    // While open, this control keeps taking clicks so the list can close.
    bool clicked = false;
    if (input_) {
        claim(id);
        const bool over = hovered(r);
        if (over) hot_ = id;
        if (over && input_->mouse_pressed &&
            (open_dropdown_ == kNoId || open_dropdown_ == id)) {
            clicked = true;
            open_dropdown_ = is_open ? kNoId : id;
            focus_ = id;
            needs_redraw_ = true;
        }
    }

    const bool over = hovered(r) && (open_dropdown_ == kNoId || open_dropdown_ == id);
    const float t   = animate(id, (over || is_open) ? 1.0f : 0.0f);

    fill_rect(r, mix(theme().panel, theme().panel_hi, t), metric::kRadius);
    stroke_rect(r, is_open ? theme().accent : mix(theme().border, theme().text_dim, t * 0.6f),
                is_open ? 1.6f : 1.0f, metric::kRadius);

    const std::wstring label =
        (index && *index >= 0 && *index < static_cast<int>(items.size())) ? items[*index] : L"";
    push_clip(inset(r, 2.0f, 1.0f));
    text(D2D1::RectF(r.left + 10.0f, r.top, r.right - 26.0f, r.bottom), label, Font::Body, theme().text);
    pop_clip();
    glyph(Glyph::ChevronDown, D2D1::RectF(r.right - 24.0f, r.top, r.right - 6.0f, r.bottom),
          theme().text_dim, 1.5f);

    if (open_dropdown_ == id) {
        popup_pending_  = true;
        popup_.anchor   = r;
        popup_.items    = items;
        popup_.selected = index ? *index : -1;
        popup_.id       = id;
    }

    (void)clicked;
    return changed;
}

bool Ctx::tabs(Id id, const D2D1_RECT_F& r, const std::vector<std::wstring>& labels, int* active) {
    if (!active || labels.empty()) return false;

    bool changed = false;
    const float pad = 16.0f;

    // Measure first so tabs size to their text rather than a fixed width.
    std::vector<float> widths(labels.size());
    float total = 0.0f;
    for (size_t i = 0; i < labels.size(); ++i) {
        widths[i] = text_width(labels[i], Font::Body) + pad * 2.0f;
        total += widths[i];
    }

    float x = r.left;
    float underline_x = r.left, underline_w = 0.0f;
    for (size_t i = 0; i < labels.size(); ++i) {
        const D2D1_RECT_F tr = D2D1::RectF(x, r.top, x + widths[i], r.bottom);
        const Id tid = id + static_cast<Id>(i) + 1;

        if (consume_click(tid, tr) && *active != static_cast<int>(i)) {
            *active = static_cast<int>(i);
            changed = true;
        }

        const bool sel  = *active == static_cast<int>(i);
        const bool over = hovered(tr) && (open_dropdown_ == kNoId);
        const float t   = animate(tid, sel ? 1.0f : (over ? 0.5f : 0.0f));

        text(tr, labels[i], Font::Body, mix(theme().text_dim, theme().text, t), Align::Center);

        if (sel) { underline_x = x; underline_w = widths[i]; }
        x += widths[i];
    }

    // Animated underline that slides to the active tab.
    const float ax = animate(id ^ 0x7777, underline_x, 20.0f);
    const float aw = animate(id ^ 0x8888, underline_w, 20.0f);
    fill_rect(D2D1::RectF(r.left, r.bottom - 1.0f, r.right, r.bottom), theme().border);
    fill_rect(D2D1::RectF(ax + 8.0f, r.bottom - 2.0f, ax + aw - 8.0f, r.bottom), theme().accent, 1.0f);
    return changed;
}

float Ctx::badge(float x, float y, float height, const std::wstring& label,
                 const D2D1_COLOR_F& colour) {
    if (label.empty()) return 0.0f;

    const float pad = 7.0f;
    const float w = text_width(label, Font::Small) + pad * 2.0f;
    const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + height);

    D2D1_COLOR_F fill = colour;
    fill.a = 0.16f;
    fill_rect(r, fill, height * 0.5f);

    D2D1_COLOR_F edge = colour;
    edge.a = 0.40f;
    stroke_rect(r, edge, 1.0f, height * 0.5f);

    text(r, label, Font::Small, colour, Align::Center);
    return w;
}

void Ctx::status_dot(float cx, float cy, float radius, const D2D1_COLOR_F& colour) {
    if (!dc_) return;
    // A dimmer halo so the dot reads at a glance without being a hard pixel.
    D2D1_COLOR_F halo = colour;
    halo.a = 0.22f;
    D2D1_ELLIPSE outer{ D2D1::Point2F(cx, cy), radius * 2.1f, radius * 2.1f };
    dc_->FillEllipse(&outer, brush(halo));

    D2D1_ELLIPSE dot{ D2D1::Point2F(cx, cy), radius, radius };
    dc_->FillEllipse(&dot, brush(colour));
}

void Ctx::section_label(const D2D1_RECT_F& r, const std::wstring& label) {
    text(r, label, Font::BodyBold, theme().text_dim);
}

void Ctx::separator(float x0, float x1, float y) {
    line(x0, y + 0.5f, x1, y + 0.5f, theme().border, 1.0f);
}

// ---- scrolling ---------------------------------------------------------
void Ctx::begin_scroll(Id id, const D2D1_RECT_F& r, ScrollState* state) {
    scroll_      = state;
    scroll_rect_ = r;
    scroll_id_   = id;
    if (!state) return;

    state->view_height = r.bottom - r.top;

    if (input_ && contains(r, input_->mouse_x, input_->mouse_y) && input_->wheel != 0.0f &&
        open_dropdown_ == kNoId) {
        state->offset -= input_->wheel * 48.0f;
        needs_redraw_ = true;
    }

    const float max_offset = std::max(0.0f, state->content_height - state->view_height);
    state->offset = std::clamp(state->offset, 0.0f, max_offset);

    // Widgets inside lay themselves out in content space, so shift the pointer
    // into the same space. A pointer outside the viewport is pushed far away so
    // clipped rows cannot register a hover or a click.
    if (input_) {
        saved_mouse_y_ = input_->mouse_y;
        scroll_active_ = true;
        input_->mouse_y = contains(r, input_->mouse_x, input_->mouse_y)
                        ? input_->mouse_y + state->offset
                        : -10000.0f;
    }

    push_clip(r);
    if (dc_)
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -state->offset));
}

void Ctx::end_scroll() {
    if (dc_) dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    pop_clip();

    if (scroll_active_ && input_) {
        input_->mouse_y = saved_mouse_y_;
        scroll_active_ = false;
    }

    if (!scroll_) return;
    const float max_offset = std::max(0.0f, scroll_->content_height - scroll_->view_height);
    if (max_offset > 0.5f) {
        // Slim scrollbar, only drawn when there is something to scroll.
        const float track_x = scroll_rect_.right - 6.0f;
        const float vh = scroll_->view_height;
        const float thumb_h = std::max(28.0f, vh * (vh / std::max(1.0f, scroll_->content_height)));
        const float t = scroll_->offset / max_offset;
        const float thumb_y = scroll_rect_.top + t * (vh - thumb_h);

        fill_rect(D2D1::RectF(track_x, thumb_y, track_x + 4.0f, thumb_y + thumb_h),
                  theme().border, 2.0f);
    }
    scroll_ = nullptr;
}

} // namespace ui
