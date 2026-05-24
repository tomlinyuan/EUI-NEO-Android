#pragma once

#include "components/scroll.h"
#include "core/dsl.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace components {

class ScrollViewBuilder {
public:
    ScrollViewBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    ScrollViewBuilder& size(float width, float height) { width_ = width; height_ = height; return *this; }
    ScrollViewBuilder& offset(float value) { offset_ = std::max(0.0f, value); return *this; }
    ScrollViewBuilder& value(float value) { return offset(value); }
    ScrollViewBuilder& gap(float value) { gap_ = std::max(0.0f, value); return *this; }
    ScrollViewBuilder& spacing(float value) { return gap(value); }
    ScrollViewBuilder& step(float value) { step_ = std::max(1.0f, value); return *this; }
    ScrollViewBuilder& scrollbarWidth(float value) { scrollbarWidth_ = std::max(0.0f, value); return *this; }
    ScrollViewBuilder& scrollbarGap(float value) { scrollbarGap_ = std::max(0.0f, value); return *this; }
    ScrollViewBuilder& zIndex(int value) { zIndex_ = value; return *this; }
    ScrollViewBuilder& z(int value) { return zIndex(value); }
    ScrollViewBuilder& style(const ScrollStyle& value) { scrollStyle_ = value; return *this; }
    ScrollViewBuilder& theme(const theme::ThemeColorTokens& tokens) { scrollStyle_ = ScrollStyle(tokens); return *this; }
    ScrollViewBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    ScrollViewBuilder& transition(float duration, core::Ease ease = core::Ease::OutCubic) {
        transition_ = core::Transition::make(duration, ease);
        return *this;
    }
    ScrollViewBuilder& onChange(std::function<void(float)> callback) {
        onChange_ = std::move(callback);
        return *this;
    }

    template <typename ComposeFn>
    ScrollViewBuilder& content(ComposeFn&& compose) {
        content_ = std::forward<ComposeFn>(compose);
        return *this;
    }

    void build() {
        const float initialContentHeight = measureContentHeight(width_, height_);
        const bool scrollable = initialContentHeight > height_;
        const float scrollWidth = scrollable ? scrollbarWidth_ : 0.0f;
        const float scrollGap = scrollable ? scrollbarGap_ : 0.0f;
        const float contentWidth = std::max(0.0f, width_ - scrollWidth - scrollGap);
        const float contentHeight = scrollable ? measureContentHeight(contentWidth, height_) : initialContentHeight;
        const float maxOffset = std::max(0.0f, contentHeight - height_);
        const float currentOffset = std::clamp(offset_, 0.0f, maxOffset);
        const std::function<void(float)> onChange = onChange_;
        const float scrollStep = step_;

        ui_.stack(id_)
            .size(width_, height_)
            .zIndex(zIndex_)
            .clip()
            .onScroll([currentOffset, maxOffset, scrollStep, onChange](const core::ScrollEvent& event) {
                if (!onChange || maxOffset <= 0.0f) {
                    return;
                }
                const float next = std::clamp(currentOffset - static_cast<float>(event.y) * scrollStep, 0.0f, maxOffset);
                onChange(next);
            })
            .onDrag([currentOffset, maxOffset, onChange](const core::dsl::DragEvent& event) {
                // Natural-direction touch scrolling: dragging the finger up
                // (negative deltaY) reveals content further down, so offset
                // grows. Pixel-level mapping — no scrollStep multiplier.
                if (!onChange || maxOffset <= 0.0f) {
                    return;
                }
                const float next = std::clamp(currentOffset - static_cast<float>(event.deltaY), 0.0f, maxOffset);
                onChange(next);
            })
            .content([&] {
                ui_.column(id_ + ".content")
                    .y(-currentOffset)
                    .width(contentWidth)
                    .height(core::SizeValue::wrapContent())
                    .gap(gap_)
                    .content([&] {
                        if (content_) {
                            content_(ui_, contentWidth, height_);
                        }
                    })
                    .build();

                if (scrollable) {
                    components::scroll(ui_, id_ + ".scroll")
                        .style(scrollStyle_)
                        .x(std::max(0.0f, width_ - scrollWidth))
                        .size(scrollWidth, height_)
                        .viewport(height_)
                        .content(contentHeight)
                        .offset(currentOffset)
                        .step(scrollStep)
                        .zIndex(zIndex_ + 1)
                        .transition(transition_)
                        .onChange(onChange)
                        .build();
                }
            })
            .build();
    }

private:
    float measureContentHeight(float contentWidth, float viewportHeight) const {
        if (!content_) {
            return viewportHeight;
        }

        core::dsl::Ui measureUi;
        measureUi.begin(id_ + ".measure");
        measureUi.column("content")
            .width(contentWidth)
            .height(core::SizeValue::wrapContent())
            .gap(gap_)
            .content([&] {
                content_(measureUi, contentWidth, viewportHeight);
            })
            .build();
        measureUi.end();
        measureUi.layout(contentWidth, 0.0f);
        const core::dsl::Element* content = measureUi.find("content");
        if (content == nullptr) {
            return viewportHeight;
        }
        return std::max(viewportHeight, content->frame.height);
    }

    core::dsl::Ui& ui_;
    std::string id_;
    ScrollStyle scrollStyle_;
    core::Transition transition_ = core::Transition::make(0.12f, core::Ease::OutCubic);
    std::function<void(float)> onChange_;
    std::function<void(core::dsl::Ui&, float, float)> content_;
    float width_ = 320.0f;
    float height_ = 220.0f;
    float offset_ = 0.0f;
    float gap_ = 0.0f;
    float step_ = 48.0f;
    float scrollbarWidth_ = 8.0f;
    float scrollbarGap_ = 16.0f;
    int zIndex_ = 0;
};

inline ScrollViewBuilder scrollView(core::dsl::Ui& ui, const std::string& id) {
    return ScrollViewBuilder(ui, id);
}

} // namespace components
