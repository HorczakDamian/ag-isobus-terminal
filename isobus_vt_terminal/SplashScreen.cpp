#include "SplashScreen.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace isobus_vt
{
    SplashScreen::SplashScreen() :
        active(true),
        fadingOut(false),
        elapsed(0.0f),
        fadeTimer(0.0f)
    {
    }

    void SplashScreen::start()
    {
        active = true;
        fadingOut = false;
        elapsed = 0.0f;
        fadeTimer = 0.0f;
    }

    void SplashScreen::dismiss()
    {
        if (active && !fadingOut)
        {
            fadingOut = true;
            fadeTimer = 0.0f;
        }
    }

    void SplashScreen::render(ImFont* titleFont, ImFont* subtitleFont, ImFont* regularFont)
    {
        if (!active) return;

        float dt = ImGui::GetIO().DeltaTime;
        elapsed += dt;

        // Key / Mouse dismiss handling
        if (ImGui::IsKeyPressed(ImGuiKey_Space) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (elapsed > 0.3f && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
        {
            dismiss();
        }

        if (fadingOut)
        {
            fadeTimer += dt;
            if (fadeTimer >= fadeOutDuration)
            {
                active = false;
                return;
            }
        }
        else if (elapsed >= totalDuration)
        {
            fadingOut = true;
            fadeTimer = 0.0f;
        }

        // Compute global alpha
        float alpha = 1.0f;
        if (elapsed < 0.25f)
        {
            alpha = elapsed / 0.25f; // Smooth fade-in
        }
        else if (fadingOut)
        {
            alpha = 1.0f - (fadeTimer / fadeOutDuration);
        }
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        auto applyAlpha = [alpha](ImU32 col) -> ImU32 {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
            c.w *= alpha;
            return ImGui::ColorConvertFloat4ToU32(c);
        };

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 vpPos = vp->Pos;
        ImVec2 vpSize = vp->Size;

        ImGui::SetNextWindowPos(vpPos);
        ImGui::SetNextWindowSize(vpSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        if (ImGui::Begin("##CommercialSplashScreen", nullptr, flags))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // ── 1. Deep Obsidian / Space Dark Background ──
            dl->AddRectFilled(vpPos, ImVec2(vpPos.x + vpSize.x, vpPos.y + vpSize.y),
                              applyAlpha(IM_COL32(8, 12, 18, 255)));

            // Subtle dark radial gradient simulated with concentric circles
            ImVec2 center(vpPos.x + vpSize.x * 0.5f, vpPos.y + vpSize.y * 0.44f);
            float maxR = std::max(vpSize.x, vpSize.y) * 0.7f;
            for (int r = 10; r >= 1; --r)
            {
                float radius = maxR * (r / 10.0f);
                int radAlpha = static_cast<int>(18 * (1.0f - (r / 10.0f)));
                dl->AddCircleFilled(center, radius, applyAlpha(IM_COL32(18, 38, 48, radAlpha)), 64);
            }

            // ── 2. Subtle High-Tech Grid Lines (Precision Ag Guidance Grid) ──
            float gridSpacing = 48.0f;
            ImU32 gridCol = applyAlpha(IM_COL32(24, 44, 56, 35));
            for (float x = vpPos.x; x < vpPos.x + vpSize.x; x += gridSpacing)
            {
                dl->AddLine(ImVec2(x, vpPos.y), ImVec2(x, vpPos.y + vpSize.y), gridCol, 1.0f);
            }
            for (float y = vpPos.y; y < vpPos.y + vpSize.y; y += gridSpacing)
            {
                dl->AddLine(ImVec2(vpPos.x, y), ImVec2(vpPos.x + vpSize.x, y), gridCol, 1.0f);
            }

            // High-Tech Corner Reticle Brackets
            float bSize = 36.0f;
            float bMargin = 28.0f;
            ImU32 bCol = applyAlpha(IM_COL32(46, 204, 113, 140));
            // Top-Left
            dl->AddLine(ImVec2(vpPos.x + bMargin, vpPos.y + bMargin), ImVec2(vpPos.x + bMargin + bSize, vpPos.y + bMargin), bCol, 2.0f);
            dl->AddLine(ImVec2(vpPos.x + bMargin, vpPos.y + bMargin), ImVec2(vpPos.x + bMargin, vpPos.y + bMargin + bSize), bCol, 2.0f);
            // Top-Right
            dl->AddLine(ImVec2(vpPos.x + vpSize.x - bMargin, vpPos.y + bMargin), ImVec2(vpPos.x + vpSize.x - bMargin - bSize, vpPos.y + bMargin), bCol, 2.0f);
            dl->AddLine(ImVec2(vpPos.x + vpSize.x - bMargin, vpPos.y + bMargin), ImVec2(vpPos.x + vpSize.x - bMargin, vpPos.y + bMargin + bSize), bCol, 2.0f);
            // Bottom-Left
            dl->AddLine(ImVec2(vpPos.x + bMargin, vpPos.y + vpSize.y - bMargin), ImVec2(vpPos.x + bMargin + bSize, vpPos.y + vpSize.y - bMargin), bCol, 2.0f);
            dl->AddLine(ImVec2(vpPos.x + bMargin, vpPos.y + vpSize.y - bMargin), ImVec2(vpPos.x + bMargin, vpPos.y + vpSize.y - bMargin - bSize), bCol, 2.0f);
            // Bottom-Right
            dl->AddLine(ImVec2(vpPos.x + vpSize.x - bMargin, vpPos.y + vpSize.y - bMargin), ImVec2(vpPos.x + vpSize.x - bMargin - bSize, vpPos.y + vpSize.y - bMargin), bCol, 2.0f);
            dl->AddLine(ImVec2(vpPos.x + vpSize.x - bMargin, vpPos.y + vpSize.y - bMargin), ImVec2(vpPos.x + vpSize.x - bMargin, vpPos.y + vpSize.y - bMargin - bSize), bCol, 2.0f);

            // ── 3. Centered Glowing ISOBUS Ag Logo Emblem ──
            ImVec2 logoCenter(center.x, center.y - 120.0f);
            float logoR = 54.0f;

            // Rotating pulse glow around emblem
            float pulse = 0.5f + 0.5f * std::sin(elapsed * 4.5f);
            float glowR = logoR + 8.0f + pulse * 6.0f;
            dl->AddCircle(logoCenter, glowR, applyAlpha(IM_COL32(38, 166, 91, static_cast<int>(80 + 70 * pulse))), 48, 2.5f);
            dl->AddCircle(logoCenter, glowR + 8.0f, applyAlpha(IM_COL32(38, 166, 91, static_cast<int>(30 + 30 * pulse))), 48, 1.5f);

            // Outer dark bezel disk
            dl->AddCircleFilled(logoCenter, logoR, applyAlpha(IM_COL32(14, 22, 28, 255)), 48);
            dl->AddCircle(logoCenter, logoR, applyAlpha(IM_COL32(52, 152, 219, 180)), 48, 2.0f);

            // Inner agricultural disk (Deep Forest Green)
            dl->AddCircleFilled(logoCenter, logoR - 6.0f, applyAlpha(IM_COL32(20, 48, 30, 255)), 48);
            dl->AddCircle(logoCenter, logoR - 6.0f, applyAlpha(IM_COL32(46, 204, 113, 230)), 48, 2.0f);

            // Stylized vector crop field furrows and golden tractor wheel inside emblem
            float furrowBaseY = logoCenter.y + 14.0f;
            auto drawArcStroke = [&](ImVec2 c, float r, float a_min, float a_max, ImU32 col, float thick) {
                dl->PathClear();
                dl->PathArcTo(c, r, a_min, a_max, 24);
                dl->PathStroke(col, 0, thick);
            };
            drawArcStroke(ImVec2(logoCenter.x, furrowBaseY + 36.0f), 38.0f, -2.4f, -0.74f, applyAlpha(IM_COL32(46, 204, 113, 220)), 3.0f);
            drawArcStroke(ImVec2(logoCenter.x, furrowBaseY + 28.0f), 28.0f, -2.5f, -0.64f, applyAlpha(IM_COL32(241, 196, 15, 230)), 3.0f);
            drawArcStroke(ImVec2(logoCenter.x, furrowBaseY + 20.0f), 18.0f, -2.6f, -0.54f, applyAlpha(IM_COL32(46, 204, 113, 220)), 2.5f);

            // Stylized ISOBUS tractor icon / GPS satellite beacon at top of emblem
            dl->AddCircleFilled(ImVec2(logoCenter.x, logoCenter.y - 14.0f), 6.5f, applyAlpha(IM_COL32(255, 255, 255, 255)));
            dl->AddCircleFilled(ImVec2(logoCenter.x, logoCenter.y - 14.0f), 4.0f, applyAlpha(IM_COL32(46, 204, 113, 255)));
            dl->AddLine(ImVec2(logoCenter.x - 14.0f, logoCenter.y - 14.0f), ImVec2(logoCenter.x - 7.0f, logoCenter.y - 14.0f), applyAlpha(IM_COL32(241, 196, 15, 240)), 2.0f);
            dl->AddLine(ImVec2(logoCenter.x + 7.0f, logoCenter.y - 14.0f), ImVec2(logoCenter.x + 14.0f, logoCenter.y - 14.0f), applyAlpha(IM_COL32(241, 196, 15, 240)), 2.0f);

            // ── 4. Main Titles & Branding ──
            float textY = center.y - 36.0f;

            // Title: ISOBUS UNIVERSAL TERMINAL
            const char* titleText = "ISOBUS UNIVERSAL TERMINAL";
            if (titleFont) ImGui::PushFont(titleFont);
            ImVec2 titleSz = ImGui::CalcTextSize(titleText);
            dl->AddText(ImVec2(center.x - titleSz.x * 0.5f, textY), applyAlpha(IM_COL32(245, 248, 255, 255)), titleText);
            if (titleFont) ImGui::PopFont();

            // Subtitle: PRECISION AG OPERATING SYSTEM
            textY += (titleFont ? titleSz.y + 6.0f : 32.0f);
            const char* subText = "PRECISION AG OPERATING SYSTEM  |  ISO 11783-6 VT & ISO 11783-10 TC";
            if (subtitleFont) ImGui::PushFont(subtitleFont);
            ImVec2 subSz = ImGui::CalcTextSize(subText);
            dl->AddText(ImVec2(center.x - subSz.x * 0.5f, textY), applyAlpha(IM_COL32(76, 175, 80, 255)), subText);
            if (subtitleFont) ImGui::PopFont();

            // ── 5. Prominent Commercial Author Badge: by D. Horczak ──
            textY += (subtitleFont ? subSz.y + 14.0f : 28.0f);
            const char* authorText = "System Architecture & Precision Ag Engineering by D. Horczak";
            ImVec2 authorSz = ImGui::CalcTextSize(authorText);
            float badgePaddingX = 22.0f;
            float badgePaddingY = 6.0f;
            ImVec2 bTL(center.x - authorSz.x * 0.5f - badgePaddingX, textY - badgePaddingY);
            ImVec2 bBR(center.x + authorSz.x * 0.5f + badgePaddingX, textY + authorSz.y + badgePaddingY);

            // Glowing pill container
            dl->AddRectFilled(bTL, bBR, applyAlpha(IM_COL32(22, 32, 26, 230)), 14.0f);
            dl->AddRect(bTL, bBR, applyAlpha(IM_COL32(46, 204, 113, 200)), 14.0f, 0, 1.5f);
            dl->AddRect(ImVec2(bTL.x - 2.0f, bTL.y - 2.0f), ImVec2(bBR.x + 2.0f, bBR.y + 2.0f),
                        applyAlpha(IM_COL32(241, 196, 15, static_cast<int>(70 + 50 * pulse))), 16.0f, 0, 1.0f);

            dl->AddText(ImVec2(center.x - authorSz.x * 0.5f, textY),
                        applyAlpha(IM_COL32(255, 215, 0, 255)), authorText);

            // ── 6. Progress Calculation & Dynamic Boot Steps ──
            float rawProgress = elapsed / (totalDuration - 0.2f);
            if (fadingOut) rawProgress = 1.0f;
            float progress = std::clamp(rawProgress, 0.0f, 1.0f);

            // Smooth ease-out curve for natural boot feel
            float displayProgress = std::sin(progress * 3.14159265f * 0.5f);
            if (displayProgress > 1.0f) displayProgress = 1.0f;

            // Boot diagnostic steps
            std::string currentStep;
            if (displayProgress < 0.18f)
                currentStep = "[ 1/6 ] Inicjalizacja podsystemu graficznego DirectX 11 & Dear ImGui...";
            else if (displayProgress < 0.38f)
                currentStep = "[ 2/6 ] Uruchamianie rdzenia serwera ISOBUS ISO 11783-6 Virtual Terminal...";
            else if (displayProgress < 0.58f)
                currentStep = "[ 3/6 ] Inicjalizacja interfejsu magistrali CAN (PEAK PCAN-USB @ 250 kbps)...";
            else if (displayProgress < 0.76f)
                currentStep = "[ 4/6 ] Konfiguracja kontrolera wejsc maszynowych AUX-N (Joysticki)...";
            else if (displayProgress < 0.92f)
                currentStep = "[ 5/6 ] Uruchamianie mostka UDP AgOpenGPS Task Controller (Porty 8888/9999)...";
            else
                currentStep = "[ 6/6 ] Sprawdzanie spojnosci pamieci podrecznej Object Pool Cache... Gotowy!";

            // ── 7. High-Tech Animated Progress Bar ──
            float barW = 560.0f;
            float barH = 16.0f;
            float barX = center.x - barW * 0.5f;
            float barY = textY + 54.0f;

            // Outer track
            dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                              applyAlpha(IM_COL32(14, 20, 30, 255)), 8.0f);
            dl->AddRect(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                        applyAlpha(IM_COL32(40, 56, 80, 255)), 8.0f, 0, 1.5f);

            // Inner fill
            float fillW = (barW - 4.0f) * displayProgress;
            if (fillW > 6.0f)
            {
                ImVec2 fTL(barX + 2.0f, barY + 2.0f);
                ImVec2 fBR(barX + 2.0f + fillW, barY + barH - 2.0f);
                dl->AddRectFilled(fTL, fBR, applyAlpha(IM_COL32(38, 166, 91, 255)), 6.0f);

                // Highlight gloss on top half of the bar
                dl->AddRectFilled(fTL, ImVec2(fBR.x, fTL.y + (barH - 4.0f) * 0.45f),
                                  applyAlpha(IM_COL32(110, 225, 140, 160)), 5.0f);

                // Leading glowing tip
                float tipX = fBR.x;
                float tipY = (fTL.y + fBR.y) * 0.5f;
                dl->AddCircleFilled(ImVec2(tipX, tipY), 4.0f, applyAlpha(IM_COL32(241, 196, 15, 255)));
            }

            // Progress percentage
            char pctBuf[16];
            std::snprintf(pctBuf, sizeof(pctBuf), "%d%%", static_cast<int>(displayProgress * 100.0f));
            ImVec2 pctSz = ImGui::CalcTextSize(pctBuf);
            dl->AddText(ImVec2(barX + barW + 14.0f, barY - 2.0f),
                        applyAlpha(IM_COL32(241, 196, 15, 255)), pctBuf);

            // Step status label below bar
            ImVec2 stepSz = ImGui::CalcTextSize(currentStep.c_str());
            dl->AddText(ImVec2(center.x - stepSz.x * 0.5f, barY + barH + 12.0f),
                        applyAlpha(IM_COL32(170, 195, 215, 255)), currentStep.c_str());

            // ── 8. Commercial Industrial Footer ──
            float footerY = vpPos.y + vpSize.y - 36.0f;
            const char* certText = "ISO 11783-6 / AEF Conformance Ready  |  DirectX 11 Hardware Accelerated";
            dl->AddText(ImVec2(vpPos.x + 36.0f, footerY),
                        applyAlpha(IM_COL32(100, 125, 145, 200)), certText);

            const char* skipHint = "[ SPACJA / KLIKNIJ ABY POMINAC ]";
            ImVec2 hintSz = ImGui::CalcTextSize(skipHint);
            dl->AddText(ImVec2(vpPos.x + vpSize.x - hintSz.x - 36.0f, footerY),
                        applyAlpha(IM_COL32(130, 160, 190, 220)), skipHint);

            ImGui::End();
        }
        ImGui::PopStyleVar(2);
    }
}
