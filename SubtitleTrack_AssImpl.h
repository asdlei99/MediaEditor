#pragma once
#include <list>
#include "SubtitleTrack.h"
extern "C"
{
    #include "libavformat/avformat.h"
    #include "libavcodec/avcodec.h"
    #include "ass/ass.h"
}

namespace DataLayer
{
    class SubtitleTrackStyle_AssImpl : public SubtitleStyle
    {
    public:
        SubtitleTrackStyle_AssImpl() = default;
        SubtitleTrackStyle_AssImpl(const ASS_Style* style);
        SubtitleTrackStyle_AssImpl(const SubtitleTrackStyle_AssImpl& a);
        SubtitleTrackStyle_AssImpl(SubtitleTrackStyle_AssImpl&&) = default;
        SubtitleTrackStyle_AssImpl& operator=(const SubtitleTrackStyle_AssImpl& a);

        std::string Name() const override { return std::string(m_name.get()); }
        std::string Font() const override { return std::string(m_assStyle.FontName); }
        double Scale() const override { return m_scale; }
        double ScaleX() const override { return m_assStyle.ScaleX; }
        double ScaleY() const override { return m_assStyle.ScaleY; }
        double Spacing() const override { return m_assStyle.Spacing; }
        double Angle() const override { return m_assStyle.Angle; }
        double OutlineWidth() const override { return m_assStyle.Outline; }
        int Alignment() const override { return m_alignment; }
        int OffsetH() const override { return m_offsetH; }
        int OffsetV() const override { return m_offsetV; }
        int Bold() const override { return m_bold; }
        int Italic() const override { return m_italic; }
        bool UnderLine() const override { return m_assStyle.Underline != 0; }
        bool StrikeOut() const override { return m_assStyle.StrikeOut != 0; }
        SubtitleColor PrimaryColor() const override { return m_primaryColor; }
        SubtitleColor SecondaryColor() const override { return m_secondaryColor; }
        SubtitleColor OutlineColor() const override { return m_outlineColor; }
        SubtitleColor BackgroundColor() const override { return m_bgColor; }

        void BuildFromAssStyle(const ASS_Style* assStyle);
        ASS_Style* GetAssStylePtr() { return &m_assStyle; }
        void SetFont(const std::string& font);
        void SetScale(double scale) { m_scale = scale; }
        void SetScaleX(double value) { m_assStyle.ScaleX = value; }
        void SetScaleY(double value) { m_assStyle.ScaleY = value; }
        void SetSpacing(double value) { m_assStyle.Spacing = value; }
        void SetAngle(double value) { m_assStyle.Angle = value; }
        void SetOutlineWidth(double value) { m_assStyle.Outline = value; }
        void SetAlignment(int value);
        void SetOffsetH(int value) { m_offsetH = value; }
        void SetOffsetV(int value) { m_offsetV = value; }
        void SetBold(int value);
        void SetItalic(int value);
        void SetUnderLine(bool enable) { m_assStyle.Underline = enable ? 1 : 0; }
        void SetStrikeOut(bool enable) { m_assStyle.StrikeOut = enable ? 1 : 0; }
        void SetPrimaryColor(const SubtitleColor& color);
        void SetSecondaryColor(const SubtitleColor& color);
        void SetOutlineColor(const SubtitleColor& color);
        void SetBackgroundColor(const SubtitleColor& color);

    private:
        ASS_Style m_assStyle;
        std::unique_ptr<char[]> m_name;
        std::unique_ptr<char[]> m_fontName;
        double m_scale{1};
        int m_offsetH{0};
        int m_offsetV{0};
        SubtitleColor m_primaryColor;
        SubtitleColor m_secondaryColor;
        SubtitleColor m_outlineColor;
        SubtitleColor m_bgColor{0,0,0,0};
        int m_bold{0};
        int m_italic{0};
        int m_alignment{2};
    };

    class SubtitleTrack_AssImpl : public SubtitleTrack
    {
    public:
        SubtitleTrack_AssImpl(int64_t id);
        ~SubtitleTrack_AssImpl();

        SubtitleTrack_AssImpl(const SubtitleTrack_AssImpl&) = delete;
        SubtitleTrack_AssImpl(SubtitleTrack_AssImpl&&) = delete;
        SubtitleTrack_AssImpl& operator=(const SubtitleTrack_AssImpl&) = delete;

        bool InitAss();

        int64_t Id() const override { return m_id; }
        uint32_t ClipCount() const override { return m_clips.size(); }
        int64_t Duration() const override { return m_duration; }
        const SubtitleStyle& DefaultStyle() const override { return m_overrideStyle; }

        bool SetFrameSize(uint32_t width, uint32_t height) override;
        bool EnableFullSizeOutput(bool enable) override;
        bool SetFont(const std::string& font) override;
        bool SetScale(double value) override;
        bool SetScaleX(double value) override;
        bool SetScaleY(double value) override;
        bool SetSpacing(double value) override;
        bool SetAngle(double value) override;
        bool SetOutlineWidth(double value) override;
        bool SetAlignment(int value) override;
        bool SetOffsetH(int value) override;
        bool SetOffsetV(int value) override;
        bool SetItalic(int value) override;
        bool SetBold(int value) override;
        bool SetUnderLine(bool enable) override;
        bool SetStrikeOut(bool enable) override;
        bool SetPrimaryColor(const SubtitleColor& color) override;
        bool SetSecondaryColor(const SubtitleColor& color) override;
        bool SetOutlineColor(const SubtitleColor& color) override;
        bool SetBackgroundColor(const SubtitleColor& color) override;
        bool SetPrimaryColor(const ImVec4& color) override;
        bool SetSecondaryColor(const ImVec4& color) override;
        bool SetOutlineColor(const ImVec4& color) override;
        bool ChangeClipTime(SubtitleClipHolder clip, int64_t startTime, int64_t duration) override;

        SubtitleClipHolder NewClip(int64_t startTime, int64_t duration) override;
        SubtitleClipHolder GetClipByTime(int64_t ms) override;
        SubtitleClipHolder GetCurrClip() override;
        SubtitleClipHolder GetPrevClip() override;
        SubtitleClipHolder GetNextClip() override;
        int32_t GetClipIndex(SubtitleClipHolder clip) const override;
        uint32_t GetCurrIndex() const override;
        bool SeekToTime(int64_t ms) override;
        bool SeekToIndex(uint32_t index) override;

        bool SaveAs(const std::string& subFilePath) override;

        std::string GetError() const override { return m_errMsg; }

        static bool Initialize();
        static void Release();
        static bool SetFontDir(const std::string& path);
        static SubtitleTrackHolder BuildFromFile(int64_t id, const std::string& url);
        static SubtitleTrackHolder NewEmptyTrack(int64_t id);

    private:
        bool ReadFile(const std::string& path);
        void ReleaseFFContext();
        SubtitleImage RenderSubtitleClip(SubtitleClip* clip);
        void ClearRenderCache();
        void ToggleOverrideStyle();

    private:
        Logger::ALogger* m_logger;
        std::string m_errMsg;
        int64_t m_id;
        std::string m_path;
        int64_t m_readPos{0};
        std::list<SubtitleClipHolder> m_clips;
        std::list<SubtitleClipHolder>::iterator m_currIter;
        int64_t m_duration{-1};
        ASS_Track* m_asstrk{nullptr};
        int m_defaultStyleIdx{-1};
        ASS_Renderer* m_assrnd{nullptr};
        uint32_t m_frmW{0}, m_frmH{0};
        bool m_outputFullSize{true};

        bool m_useOverrideStyle{false};
        SubtitleTrackStyle_AssImpl m_overrideStyle;

        AVFormatContext* m_pAvfmtCtx{nullptr};
        AVCodecContext* m_pAvCdcCtx{nullptr};

        static ASS_Library* s_asslib;
    };
}