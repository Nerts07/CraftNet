#pragma once
#include "text.hpp"
#include "psp_dialog.hpp"
#include <raylib.h>
#include <algorithm>
#include <string>
#if defined(__PSP__)
#include <psputility.h>
#endif

namespace craft
{
class Keyboard final
{
  public:
    bool active = false;
    bool completed = false;
    bool accepted = false;
    std::string result;

    bool open(const char *title, const std::string &value, int maximum, bool ascii = false, bool digits = false)
    {
#if defined(__PSP__)
        if (active)
        {
            return false;
        }
        data_ = {};
        params_ = {};
        std::fill(description_, description_ + 64, 0);
        std::fill(initial_, initial_ + 257, 0);
        std::fill(output_, output_ + 257, 0);
        for (int i = 0; i < 63 && title[i]; ++i)
        {
            description_[i] = static_cast<unsigned char>(title[i]);
        }
        const char *cursor = value.c_str();
        int at = 0;
        while (*cursor && at < 254)
        {
            int used = 0;
            int code = GetCodepointNext(cursor, &used);
            if (used <= 0)
            {
                break;
            }
            cursor += used;
            if (code >= 0x10000)
            {
                initial_[at++] = static_cast<unsigned short>(0xd800 + ((code - 0x10000) >> 10));
                initial_[at++] = static_cast<unsigned short>(0xdc00 + ((code - 0x10000) & 1023));
            }
            else
            {
                initial_[at++] = static_cast<unsigned short>(code);
            }
        }
        data_.language = ascii ? PSP_UTILITY_OSK_LANGUAGE_ENGLISH : PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
        data_.inputtype = digits ? PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT : ascii ? 15 : PSP_UTILITY_OSK_INPUTTYPE_ALL;
        data_.lines = 1;
        data_.desc = description_;
        data_.intext = initial_;
        data_.outtext = output_;
        data_.outtextlength = 257;
        data_.outtextlimit = std::clamp(maximum, 1, 256);
        params_.base.size = sizeof(params_);
        params_.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
        sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params_.base.language);
        params_.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
        params_.base.graphicsThread = 17;
        params_.base.accessThread = 19;
        params_.base.fontThread = 18;
        params_.base.soundThread = 16;
        params_.datacount = 1;
        params_.data = &data_;
        shuttingDown_ = false;
        completed = false;
        accepted = false;
        result.clear();
        active = sceUtilityOskInitStart(&params_) >= 0;
        return active;
#else
        (void)title;
        (void)value;
        (void)maximum;
        (void)ascii;
        (void)digits;
        return false;
#endif
    }

    void update()
    {
#if defined(__PSP__)
        if (!active)
        {
            return;
        }
        const int status = sceUtilityOskGetStatus();
        if (status == PSP_UTILITY_DIALOG_VISIBLE)
        {
            drawPspDialog(sceUtilityOskUpdate);
        }
        else if (status == PSP_UTILITY_DIALOG_QUIT && !shuttingDown_)
        {
            shuttingDown_ = sceUtilityOskShutdownStart() >= 0;
        }
        else if (shuttingDown_ && (status == PSP_UTILITY_DIALOG_FINISHED || status == PSP_UTILITY_DIALOG_NONE))
        {
            accepted = data_.result != PSP_UTILITY_OSK_RESULT_CANCELLED && params_.base.result == 0;
            if (accepted)
            {
                for (int i = 0; i < 256 && output_[i]; ++i)
                {
                    unsigned code = output_[i];
                    if (code >= 0xd800 && code <= 0xdbff && i + 1 < 256 && output_[i + 1] >= 0xdc00 &&
                        output_[i + 1] <= 0xdfff)
                    {
                        code = 0x10000 + ((code - 0xd800) << 10) + output_[++i] - 0xdc00;
                    }
                    if (code >= 32 && code != 127)
                    {
                        appendUtf8(result, code);
                    }
                }
            }
            completed = true;
            active = false;
        }
        else if (status < 0)
        {
            completed = true;
            active = false;
        }
#endif
    }

  private:
#if defined(__PSP__)
    SceUtilityOskData data_{};
    SceUtilityOskParams params_{};
    unsigned short description_[64]{};
    unsigned short initial_[257]{};
    unsigned short output_[257]{};
    bool shuttingDown_ = false;
#endif
};
} // namespace craft
