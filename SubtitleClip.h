#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include "immat.h"

namespace DataLayer
{
    enum SubtitleType
    {
        UNKNOWN = 0,
        TEXT,
        BITMAP,
        ASS,
    };

    class SubtitleImage
    {
    public:
        struct Rect
        {
            int32_t x;
            int32_t y;
            int32_t w;
            int32_t h;
        };

        SubtitleImage() = default;
        SubtitleImage(ImGui::ImMat& image, const Rect& area);

        ImGui::ImMat Image() const { return m_image; }
        Rect Area() const { return m_area; }
        bool Valid() const { return !m_image.empty(); }
        void Invalidate() { m_image.release(); }

    private:
        ImGui::ImMat m_image;
        Rect m_area;
    };

    class SubtitleClip;
    using SubtitleClipHolder = std::shared_ptr<SubtitleClip>;
    using RenderCallback = std::function<SubtitleImage(SubtitleClip*)>;

    class SubtitleClip
    {
    public:
        SubtitleClip(SubtitleType type, int readOrder, int64_t startTime, int64_t duration, const char* text);
        SubtitleClip(SubtitleType type, int readOrder, int64_t startTime, int64_t duration, const std::string& text);
        SubtitleClip(SubtitleType type, int readOrder, int64_t startTime, int64_t duration, SubtitleImage& image);
        SubtitleClip(const SubtitleClip&) = delete;
        SubtitleClip(SubtitleClip&&) = delete;
        SubtitleClip& operator=(const SubtitleClip&) = delete;

        struct Color
        {
            Color() = default;
            Color(float _r, float _g, float _b, float _a) : r(_r), g(_g), b(_b), a(_a) {}
            float r{1};
            float g{1};
            float b{1};
            float a{1};
        };

        void SetRenderCallback(RenderCallback renderCb) { m_renderCb = renderCb; }
        // bool SetFont(const std::string& font);
        // bool SetScale(double scale);
        // void SetTextColor(const Color& color);
        void SetBackgroundColor(const Color& color);
        void InvalidateImage() { m_image.Invalidate(); }
        void SetText(const std::string& text) { m_text = text; }
        void SetReadOrder(int readOrder) { m_readOrder = readOrder; }
        void SetStartTime(int64_t startTime) { m_startTime = startTime; }
        void SetDuration(int64_t duration) { m_duration = duration; }

        SubtitleType Type() const { return m_type; }
        int ReadOrder() const { return m_readOrder; }
        // std::string Font() const { return m_font; }
        // double FontScale() const { return m_fontScale; }
        // Color TextColor() const { return m_textColor; }
        Color BackgroundColor() const { return m_bgColor; }
        int64_t StartTime() const { return m_startTime; }
        int64_t Duration() const { return m_duration; }
        int64_t EndTime() const { return m_startTime+m_duration; }
        std::string Text() const { return m_text; }
        SubtitleImage Image();

    private:
        SubtitleType m_type;
        int m_readOrder;
        std::string m_font;
        double m_fontScale;
        Color m_textColor;
        Color m_bgColor{0, 0, 0, 0};
        int64_t m_startTime;
        int64_t m_duration;
        std::string m_text;
        SubtitleImage m_image;
        RenderCallback m_renderCb;
    };
}